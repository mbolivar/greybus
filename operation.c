/*
 * Greybus operations
 *
 * Copyright 2014-2015 Google Inc.
 * Copyright 2014-2015 Linaro Ltd.
 *
 * Released under the GPLv2 only.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "greybus.h"

static struct kmem_cache *gb_operation_cache;
static struct kmem_cache *gb_message_cache;

/* Workqueue to handle Greybus operation completions. */
static struct workqueue_struct *gb_operation_completion_wq;

/* Wait queue for synchronous cancellations. */
static DECLARE_WAIT_QUEUE_HEAD(gb_operation_cancellation_queue);

/*
 * Protects updates to operation->errno.
 */
static DEFINE_SPINLOCK(gb_operations_lock);

static int gb_operation_response_send(struct gb_operation *operation,
					int errno);

/*
 * Increment operation active count and add to connection list unless the
 * connection is going away.
 *
 * Caller holds operation reference.
 */
static int gb_operation_get_active(struct gb_operation *operation)
{
	struct gb_connection *connection = operation->connection;
	unsigned long flags;

	spin_lock_irqsave(&connection->lock, flags);

	if (connection->state != GB_CONNECTION_STATE_ENABLED) {
		spin_unlock_irqrestore(&connection->lock, flags);
		return -ENOTCONN;
	}

	if (operation->active++ == 0)
		list_add_tail(&operation->links, &connection->operations);

	spin_unlock_irqrestore(&connection->lock, flags);

	return 0;
}

/* Caller holds operation reference. */
static void gb_operation_put_active(struct gb_operation *operation)
{
	struct gb_connection *connection = operation->connection;
	unsigned long flags;

	spin_lock_irqsave(&connection->lock, flags);
	if (--operation->active == 0) {
		list_del(&operation->links);
		if (atomic_read(&operation->waiters))
			wake_up(&gb_operation_cancellation_queue);
	}
	spin_unlock_irqrestore(&connection->lock, flags);
}

static bool gb_operation_is_active(struct gb_operation *operation)
{
	struct gb_connection *connection = operation->connection;
	unsigned long flags;
	bool ret;

	spin_lock_irqsave(&connection->lock, flags);
	ret = operation->active;
	spin_unlock_irqrestore(&connection->lock, flags);

	return ret;
}

/*
 * Set an operation's result.
 *
 * Initially an outgoing operation's errno value is -EBADR.
 * If no error occurs before sending the request message the only
 * valid value operation->errno can be set to is -EINPROGRESS,
 * indicating the request has been (or rather is about to be) sent.
 * At that point nobody should be looking at the result until the
 * response arrives.
 *
 * The first time the result gets set after the request has been
 * sent, that result "sticks."  That is, if two concurrent threads
 * race to set the result, the first one wins.  The return value
 * tells the caller whether its result was recorded; if not the
 * caller has nothing more to do.
 *
 * The result value -EILSEQ is reserved to signal an implementation
 * error; if it's ever observed, the code performing the request has
 * done something fundamentally wrong.  It is an error to try to set
 * the result to -EBADR, and attempts to do so result in a warning,
 * and -EILSEQ is used instead.  Similarly, the only valid result
 * value to set for an operation in initial state is -EINPROGRESS.
 * Attempts to do otherwise will also record a (successful) -EILSEQ
 * operation result.
 */
static bool gb_operation_result_set(struct gb_operation *operation, int result)
{
	unsigned long flags;
	int prev;

	if (result == -EINPROGRESS) {
		/*
		 * -EINPROGRESS is used to indicate the request is
		 * in flight.  It should be the first result value
		 * set after the initial -EBADR.  Issue a warning
		 * and record an implementation error if it's
		 * set at any other time.
		 */
		spin_lock_irqsave(&gb_operations_lock, flags);
		prev = operation->errno;
		if (prev == -EBADR)
			operation->errno = result;
		else
			operation->errno = -EILSEQ;
		spin_unlock_irqrestore(&gb_operations_lock, flags);
		WARN_ON(prev != -EBADR);

		return true;
	}

	/*
	 * The first result value set after a request has been sent
	 * will be the final result of the operation.  Subsequent
	 * attempts to set the result are ignored.
	 *
	 * Note that -EBADR is a reserved "initial state" result
	 * value.  Attempts to set this value result in a warning,
	 * and the result code is set to -EILSEQ instead.
	 */
	if (WARN_ON(result == -EBADR))
		result = -EILSEQ; /* Nobody should be setting -EBADR */

	spin_lock_irqsave(&gb_operations_lock, flags);
	prev = operation->errno;
	if (prev == -EINPROGRESS)
		operation->errno = result;	/* First and final result */
	spin_unlock_irqrestore(&gb_operations_lock, flags);

	return prev == -EINPROGRESS;
}

int gb_operation_result(struct gb_operation *operation)
{
	int result = operation->errno;

	WARN_ON(result == -EBADR);
	WARN_ON(result == -EINPROGRESS);

	return result;
}
EXPORT_SYMBOL_GPL(gb_operation_result);

/*
 * Looks up an outgoing operation on a connection and returns a refcounted
 * pointer if found, or NULL otherwise.
 */
static struct gb_operation *
gb_operation_find_outgoing(struct gb_connection *connection, u16 operation_id)
{
	struct gb_operation *operation;
	unsigned long flags;
	bool found = false;

	spin_lock_irqsave(&connection->lock, flags);
	list_for_each_entry(operation, &connection->operations, links)
		if (operation->id == operation_id &&
				!gb_operation_is_incoming(operation)) {
			gb_operation_get(operation);
			found = true;
			break;
		}
	spin_unlock_irqrestore(&connection->lock, flags);

	return found ? operation : NULL;
}

static int gb_message_send(struct gb_message *message, gfp_t gfp)
{
	struct gb_connection *connection = message->operation->connection;

	return connection->hd->driver->message_send(connection->hd,
					connection->hd_cport_id,
					message,
					gfp);
}

/*
 * Cancel a message we have passed to the host device layer to be sent.
 */
static void gb_message_cancel(struct gb_message *message)
{
	struct greybus_host_device *hd = message->operation->connection->hd;

	hd->driver->message_cancel(message);
}

static void gb_operation_request_handle(struct gb_operation *operation)
{
	struct gb_protocol *protocol = operation->connection->protocol;
	int status;
	int ret;

	if (!protocol)
		return;

	if (protocol->request_recv) {
		status = protocol->request_recv(operation->type, operation);
	} else {
		dev_err(&operation->connection->dev,
			"unexpected incoming request type 0x%02hhx\n",
			operation->type);

		status = -EPROTONOSUPPORT;
	}

	ret = gb_operation_response_send(operation, status);
	if (ret) {
		dev_err(&operation->connection->dev,
			"failed to send response %d for type 0x%02hhx: %d\n",
			status, operation->type, ret);
			return;
	}
}

/*
 * Process operation work.
 *
 * For incoming requests, call the protocol request handler. The operation
 * result should be -EINPROGRESS at this point.
 *
 * For outgoing requests, the operation result value should have
 * been set before queueing this.  The operation callback function
 * allows the original requester to know the request has completed
 * and its result is available.
 */
static void gb_operation_work(struct work_struct *work)
{
	struct gb_operation *operation;

	operation = container_of(work, struct gb_operation, work);

	if (gb_operation_is_incoming(operation))
		gb_operation_request_handle(operation);
	else
		operation->callback(operation);

	gb_operation_put_active(operation);
	gb_operation_put(operation);
}

static void gb_operation_message_init(struct greybus_host_device *hd,
				struct gb_message *message, u16 operation_id,
				size_t payload_size, u8 type)
{
	struct gb_operation_msg_hdr *header;

	header = message->buffer;

	message->header = header;
	message->payload = payload_size ? header + 1 : NULL;
	message->payload_size = payload_size;

	/*
	 * The type supplied for incoming message buffers will be
	 * 0x00.  Such buffers will be overwritten by arriving data
	 * so there's no need to initialize the message header.
	 */
	if (type != GB_OPERATION_TYPE_INVALID) {
		u16 message_size = (u16)(sizeof(*header) + payload_size);

		/*
		 * For a request, the operation id gets filled in
		 * when the message is sent.  For a response, it
		 * will be copied from the request by the caller.
		 *
		 * The result field in a request message must be
		 * zero.  It will be set just prior to sending for
		 * a response.
		 */
		header->size = cpu_to_le16(message_size);
		header->operation_id = 0;
		header->type = type;
		header->result = 0;
	}
}

/*
 * Allocate a message to be used for an operation request or response.
 * Both types of message contain a common header.  The request message
 * for an outgoing operation is outbound, as is the response message
 * for an incoming operation.  The message header for an outbound
 * message is partially initialized here.
 *
 * The headers for inbound messages don't need to be initialized;
 * they'll be filled in by arriving data.
 *
 * Our message buffers have the following layout:
 *	message header  \_ these combined are
 *	message payload /  the message size
 */
static struct gb_message *
gb_operation_message_alloc(struct greybus_host_device *hd, u8 type,
				size_t payload_size, gfp_t gfp_flags)
{
	struct gb_message *message;
	struct gb_operation_msg_hdr *header;
	size_t message_size = payload_size + sizeof(*header);

	if (message_size > hd->buffer_size_max) {
		pr_warn("requested message size too big (%zu > %zu)\n",
				message_size, hd->buffer_size_max);
		return NULL;
	}

	/* Allocate the message structure and buffer. */
	message = kmem_cache_zalloc(gb_message_cache, gfp_flags);
	if (!message)
		return NULL;

	message->buffer = kzalloc(message_size, gfp_flags);
	if (!message->buffer)
		goto err_free_message;

	/* Initialize the message.  Operation id is filled in later. */
	gb_operation_message_init(hd, message, 0, payload_size, type);

	return message;

err_free_message:
	kmem_cache_free(gb_message_cache, message);

	return NULL;
}

static void gb_operation_message_free(struct gb_message *message)
{
	kfree(message->buffer);
	kmem_cache_free(gb_message_cache, message);
}

/*
 * Map an enum gb_operation_status value (which is represented in a
 * message as a single byte) to an appropriate Linux negative errno.
 */
static int gb_operation_status_map(u8 status)
{
	switch (status) {
	case GB_OP_SUCCESS:
		return 0;
	case GB_OP_INTERRUPTED:
		return -EINTR;
	case GB_OP_TIMEOUT:
		return -ETIMEDOUT;
	case GB_OP_NO_MEMORY:
		return -ENOMEM;
	case GB_OP_PROTOCOL_BAD:
		return -EPROTONOSUPPORT;
	case GB_OP_OVERFLOW:
		return -EMSGSIZE;
	case GB_OP_INVALID:
		return -EINVAL;
	case GB_OP_RETRY:
		return -EAGAIN;
	case GB_OP_NONEXISTENT:
		return -ENODEV;
	case GB_OP_MALFUNCTION:
		return -EILSEQ;
	case GB_OP_UNKNOWN_ERROR:
	default:
		return -EIO;
	}
}

/*
 * Map a Linux errno value (from operation->errno) into the value
 * that should represent it in a response message status sent
 * over the wire.  Returns an enum gb_operation_status value (which
 * is represented in a message as a single byte).
 */
static u8 gb_operation_errno_map(int errno)
{
	switch (errno) {
	case 0:
		return GB_OP_SUCCESS;
	case -EINTR:
		return GB_OP_INTERRUPTED;
	case -ETIMEDOUT:
		return GB_OP_TIMEOUT;
	case -ENOMEM:
		return GB_OP_NO_MEMORY;
	case -EPROTONOSUPPORT:
		return GB_OP_PROTOCOL_BAD;
	case -EMSGSIZE:
		return GB_OP_OVERFLOW;	/* Could be underflow too */
	case -EINVAL:
		return GB_OP_INVALID;
	case -EAGAIN:
		return GB_OP_RETRY;
	case -EILSEQ:
		return GB_OP_MALFUNCTION;
	case -ENODEV:
		return GB_OP_NONEXISTENT;
	case -EIO:
	default:
		return GB_OP_UNKNOWN_ERROR;
	}
}

bool gb_operation_response_alloc(struct gb_operation *operation,
					size_t response_size, gfp_t gfp)
{
	struct greybus_host_device *hd = operation->connection->hd;
	struct gb_operation_msg_hdr *request_header;
	struct gb_message *response;
	u8 type;

	type = operation->type | GB_MESSAGE_TYPE_RESPONSE;
	response = gb_operation_message_alloc(hd, type, response_size, gfp);
	if (!response)
		return false;
	response->operation = operation;

	/*
	 * Size and type get initialized when the message is
	 * allocated.  The errno will be set before sending.  All
	 * that's left is the operation id, which we copy from the
	 * request message header (as-is, in little-endian order).
	 */
	request_header = operation->request->header;
	response->header->operation_id = request_header->operation_id;
	operation->response = response;

	return true;
}
EXPORT_SYMBOL_GPL(gb_operation_response_alloc);

/*
 * Create a Greybus operation to be sent over the given connection.
 * The request buffer will be big enough for a payload of the given
 * size.
 *
 * For outgoing requests, the request message's header will be
 * initialized with the type of the request and the message size.
 * Outgoing operations must also specify the response buffer size,
 * which must be sufficient to hold all expected response data.  The
 * response message header will eventually be overwritten, so there's
 * no need to initialize it here.
 *
 * Request messages for incoming operations can arrive in interrupt
 * context, so they must be allocated with GFP_ATOMIC.  In this case
 * the request buffer will be immediately overwritten, so there is
 * no need to initialize the message header.  Responsibility for
 * allocating a response buffer lies with the incoming request
 * handler for a protocol.  So we don't allocate that here.
 *
 * Returns a pointer to the new operation or a null pointer if an
 * error occurs.
 */
static struct gb_operation *
gb_operation_create_common(struct gb_connection *connection, u8 type,
				size_t request_size, size_t response_size,
				unsigned long op_flags, gfp_t gfp_flags)
{
	struct greybus_host_device *hd = connection->hd;
	struct gb_operation *operation;

	operation = kmem_cache_zalloc(gb_operation_cache, gfp_flags);
	if (!operation)
		return NULL;
	operation->connection = connection;

	operation->request = gb_operation_message_alloc(hd, type, request_size,
							gfp_flags);
	if (!operation->request)
		goto err_cache;
	operation->request->operation = operation;

	/* Allocate the response buffer for outgoing operations */
	if (!(op_flags & GB_OPERATION_FLAG_INCOMING)) {
		if (!gb_operation_response_alloc(operation, response_size,
						 gfp_flags)) {
			goto err_request;
		}
	}

	operation->flags = op_flags;
	operation->type = type;
	operation->errno = -EBADR;  /* Initial value--means "never set" */

	INIT_WORK(&operation->work, gb_operation_work);
	init_completion(&operation->completion);
	kref_init(&operation->kref);
	atomic_set(&operation->waiters, 0);

	return operation;

err_request:
	gb_operation_message_free(operation->request);
err_cache:
	kmem_cache_free(gb_operation_cache, operation);

	return NULL;
}

/*
 * Create a new operation associated with the given connection.  The
 * request and response sizes provided are the number of bytes
 * required to hold the request/response payload only.  Both of
 * these are allowed to be 0.  Note that 0x00 is reserved as an
 * invalid operation type for all protocols, and this is enforced
 * here.
 */
struct gb_operation *gb_operation_create(struct gb_connection *connection,
					u8 type, size_t request_size,
					size_t response_size,
					gfp_t gfp)
{
	if (WARN_ON_ONCE(type == GB_OPERATION_TYPE_INVALID))
		return NULL;
	if (WARN_ON_ONCE(type & GB_MESSAGE_TYPE_RESPONSE))
		type &= ~GB_MESSAGE_TYPE_RESPONSE;

	return gb_operation_create_common(connection, type,
					request_size, response_size, 0, gfp);
}
EXPORT_SYMBOL_GPL(gb_operation_create);

size_t gb_operation_get_payload_size_max(struct gb_connection *connection)
{
	struct greybus_host_device *hd = connection->hd;

	return hd->buffer_size_max - sizeof(struct gb_operation_msg_hdr);
}
EXPORT_SYMBOL_GPL(gb_operation_get_payload_size_max);

static struct gb_operation *
gb_operation_create_incoming(struct gb_connection *connection, u16 id,
				u8 type, void *data, size_t size)
{
	struct gb_operation *operation;
	size_t request_size;
	unsigned long flags = GB_OPERATION_FLAG_INCOMING;

	/* Caller has made sure we at least have a message header. */
	request_size = size - sizeof(struct gb_operation_msg_hdr);

	if (!id)
		flags |= GB_OPERATION_FLAG_UNIDIRECTIONAL;

	operation = gb_operation_create_common(connection, type,
					request_size, 0, flags, GFP_ATOMIC);
	if (!operation)
		return NULL;

	operation->id = id;
	memcpy(operation->request->header, data, size);

	return operation;
}

/*
 * Get an additional reference on an operation.
 */
void gb_operation_get(struct gb_operation *operation)
{
	kref_get(&operation->kref);
}
EXPORT_SYMBOL_GPL(gb_operation_get);

/*
 * Destroy a previously created operation.
 */
static void _gb_operation_destroy(struct kref *kref)
{
	struct gb_operation *operation;

	operation = container_of(kref, struct gb_operation, kref);

	if (operation->response)
		gb_operation_message_free(operation->response);
	gb_operation_message_free(operation->request);

	kmem_cache_free(gb_operation_cache, operation);
}

/*
 * Drop a reference on an operation, and destroy it when the last
 * one is gone.
 */
void gb_operation_put(struct gb_operation *operation)
{
	if (WARN_ON(!operation))
		return;

	kref_put(&operation->kref, _gb_operation_destroy);
}
EXPORT_SYMBOL_GPL(gb_operation_put);

/* Tell the requester we're done */
static void gb_operation_sync_callback(struct gb_operation *operation)
{
	complete(&operation->completion);
}

/*
 * Send an operation request message. The caller has filled in any payload so
 * the request message is ready to go. The callback function supplied will be
 * called when the response message has arrived indicating the operation is
 * complete. In that case, the callback function is responsible for fetching
 * the result of the operation using gb_operation_result() if desired, and
 * dropping the initial reference to the operation.
 */
int gb_operation_request_send(struct gb_operation *operation,
				gb_operation_callback callback,
				gfp_t gfp)
{
	struct gb_connection *connection = operation->connection;
	struct gb_operation_msg_hdr *header;
	unsigned int cycle;
	int ret;

	if (!callback)
		return -EINVAL;
	/*
	 * Record the callback function, which is executed in
	 * non-atomic (workqueue) context when the final result
	 * of an operation has been set.
	 */
	operation->callback = callback;

	/*
	 * Assign the operation's id, and store it in the request header.
	 * Zero is a reserved operation id.
	 */
	cycle = (unsigned int)atomic_inc_return(&connection->op_cycle);
	operation->id = (u16)(cycle % U16_MAX + 1);
	header = operation->request->header;
	header->operation_id = cpu_to_le16(operation->id);

	gb_operation_result_set(operation, -EINPROGRESS);

	/*
	 * Get an extra reference on the operation. It'll be dropped when the
	 * operation completes.
	 */
	gb_operation_get(operation);
	ret = gb_operation_get_active(operation);
	if (ret)
		goto err_put;

	ret = gb_message_send(operation->request, gfp);
	if (ret)
		goto err_put_active;

	return 0;

err_put_active:
	gb_operation_put_active(operation);
err_put:
	gb_operation_put(operation);

	return ret;
}
EXPORT_SYMBOL_GPL(gb_operation_request_send);

/*
 * Send a synchronous operation.  This function is expected to
 * block, returning only when the response has arrived, (or when an
 * error is detected.  The return value is the result of the
 * operation.
 */
int gb_operation_request_send_sync_timeout(struct gb_operation *operation,
						unsigned int timeout)
{
	int ret;
	unsigned long timeout_jiffies;

	ret = gb_operation_request_send(operation, gb_operation_sync_callback,
					GFP_KERNEL);
	if (ret)
		return ret;

	if (timeout)
		timeout_jiffies = msecs_to_jiffies(timeout);
	else
		timeout_jiffies = MAX_SCHEDULE_TIMEOUT;

	ret = wait_for_completion_interruptible_timeout(&operation->completion,
							timeout_jiffies);
	if (ret < 0) {
		/* Cancel the operation if interrupted */
		gb_operation_cancel(operation, -ECANCELED);
	} else if (ret == 0) {
		/* Cancel the operation if op timed out */
		gb_operation_cancel(operation, -ETIMEDOUT);
	}

	return gb_operation_result(operation);
}
EXPORT_SYMBOL_GPL(gb_operation_request_send_sync_timeout);

/*
 * Send a response for an incoming operation request.  A non-zero
 * errno indicates a failed operation.
 *
 * If there is any response payload, the incoming request handler is
 * responsible for allocating the response message.  Otherwise the
 * it can simply supply the result errno; this function will
 * allocate the response message if necessary.
 */
static int gb_operation_response_send(struct gb_operation *operation,
					int errno)
{
	struct gb_connection *connection = operation->connection;
	int ret;

	if (!operation->response &&
			!gb_operation_is_unidirectional(operation)) {
		if (!gb_operation_response_alloc(operation, 0, GFP_KERNEL))
			return -ENOMEM;
	}

	/* Record the result */
	if (!gb_operation_result_set(operation, errno)) {
		dev_err(&connection->dev, "request result already set\n");
		return -EIO;	/* Shouldn't happen */
	}

	/* Sender of request does not care about response. */
	if (gb_operation_is_unidirectional(operation))
		return 0;

	/* Reference will be dropped when message has been sent. */
	gb_operation_get(operation);
	ret = gb_operation_get_active(operation);
	if (ret)
		goto err_put;

	/* Fill in the response header and send it */
	operation->response->header->result = gb_operation_errno_map(errno);

	ret = gb_message_send(operation->response, GFP_KERNEL);
	if (ret)
		goto err_put_active;

	return 0;

err_put_active:
	gb_operation_put_active(operation);
err_put:
	gb_operation_put(operation);

	return ret;
}

/*
 * This function is called when a message send request has completed.
 */
void greybus_message_sent(struct greybus_host_device *hd,
					struct gb_message *message, int status)
{
	struct gb_operation *operation = message->operation;
	struct gb_connection *connection = operation->connection;

	/*
	 * If the message was a response, we just need to drop our
	 * reference to the operation.  If an error occurred, report
	 * it.
	 *
	 * For requests, if there's no error, there's nothing more
	 * to do until the response arrives.  If an error occurred
	 * attempting to send it, record that as the result of
	 * the operation and schedule its completion.
	 */
	if (message == operation->response) {
		if (status) {
			dev_err(&connection->dev,
				"error sending response type 0x%02hhx: %d\n",
				operation->type, status);
		}
		gb_operation_put_active(operation);
		gb_operation_put(operation);
	} else if (status) {
		if (gb_operation_result_set(operation, status)) {
			queue_work(gb_operation_completion_wq,
					&operation->work);
		}
	}
}
EXPORT_SYMBOL_GPL(greybus_message_sent);

/*
 * We've received data on a connection, and it doesn't look like a
 * response, so we assume it's a request.
 *
 * This is called in interrupt context, so just copy the incoming
 * data into the request buffer and handle the rest via workqueue.
 */
static void gb_connection_recv_request(struct gb_connection *connection,
				       u16 operation_id, u8 type,
				       void *data, size_t size)
{
	struct gb_operation *operation;
	int ret;

	operation = gb_operation_create_incoming(connection, operation_id,
						type, data, size);
	if (!operation) {
		dev_err(&connection->dev, "can't create operation\n");
		return;		/* XXX Respond with pre-allocated ENOMEM */
	}

	ret = gb_operation_get_active(operation);
	if (ret) {
		gb_operation_put(operation);
		return;
	}

	/*
	 * The initial reference to the operation will be dropped when the
	 * request handler returns.
	 */
	if (gb_operation_result_set(operation, -EINPROGRESS))
		queue_work(connection->wq, &operation->work);
}

/*
 * We've received data that appears to be an operation response
 * message.  Look up the operation, and record that we've received
 * its response.
 *
 * This is called in interrupt context, so just copy the incoming
 * data into the response buffer and handle the rest via workqueue.
 */
static void gb_connection_recv_response(struct gb_connection *connection,
			u16 operation_id, u8 result, void *data, size_t size)
{
	struct gb_operation *operation;
	struct gb_message *message;
	int errno = gb_operation_status_map(result);
	size_t message_size;

	operation = gb_operation_find_outgoing(connection, operation_id);
	if (!operation) {
		dev_err(&connection->dev, "operation not found\n");
		return;
	}

	message = operation->response;
	message_size = sizeof(*message->header) + message->payload_size;
	if (!errno && size != message_size) {
		dev_err(&connection->dev, "bad message (0x%02hhx) size (%zu != %zu)\n",
			message->header->type, size, message_size);
		errno = -EMSGSIZE;
	}

	/* We must ignore the payload if a bad status is returned */
	if (errno)
		size = sizeof(*message->header);

	/* The rest will be handled in work queue context */
	if (gb_operation_result_set(operation, errno)) {
		memcpy(message->header, data, size);
		queue_work(gb_operation_completion_wq, &operation->work);
	}

	gb_operation_put(operation);
}

/*
 * Handle data arriving on a connection.  As soon as we return the
 * supplied data buffer will be reused (so unless we do something
 * with, it's effectively dropped).
 */
void gb_connection_recv(struct gb_connection *connection,
				void *data, size_t size)
{
	struct gb_operation_msg_hdr header;
	size_t msg_size;
	u16 operation_id;

	if (connection->state != GB_CONNECTION_STATE_ENABLED) {
		dev_err(&connection->dev, "dropping %zu received bytes\n",
			size);
		return;
	}

	if (size < sizeof(header)) {
		dev_err(&connection->dev, "message too small\n");
		return;
	}

	/* Use memcpy as data may be unaligned */
	memcpy(&header, data, sizeof(header));
	msg_size = le16_to_cpu(header.size);
	if (size < msg_size) {
		dev_err(&connection->dev,
			"incomplete message received for type 0x%02hhx: 0x%04x (%zu < %zu)\n",
			header.type, le16_to_cpu(header.operation_id), size,
			msg_size);
		return;		/* XXX Should still complete operation */
	}

	operation_id = le16_to_cpu(header.operation_id);
	if (header.type & GB_MESSAGE_TYPE_RESPONSE)
		gb_connection_recv_response(connection, operation_id,
						header.result, data, msg_size);
	else
		gb_connection_recv_request(connection, operation_id,
						header.type, data, msg_size);
}

/*
 * Cancel an outgoing operation synchronously, and record the given error to
 * indicate why.
 */
void gb_operation_cancel(struct gb_operation *operation, int errno)
{
	if (WARN_ON(gb_operation_is_incoming(operation)))
		return;

	if (gb_operation_result_set(operation, errno)) {
		gb_message_cancel(operation->request);
		queue_work(gb_operation_completion_wq, &operation->work);
	}

	atomic_inc(&operation->waiters);
	wait_event(gb_operation_cancellation_queue,
			!gb_operation_is_active(operation));
	atomic_dec(&operation->waiters);
}
EXPORT_SYMBOL_GPL(gb_operation_cancel);

/*
 * Cancel an incoming operation synchronously. Called during connection tear
 * down.
 */
void gb_operation_cancel_incoming(struct gb_operation *operation, int errno)
{
	if (WARN_ON(!gb_operation_is_incoming(operation)))
		return;

	if (!gb_operation_is_unidirectional(operation)) {
		/*
		 * Make sure the request handler has submitted the response
		 * before cancelling it.
		 */
		flush_work(&operation->work);
		if (!gb_operation_result_set(operation, errno))
			gb_message_cancel(operation->response);
	}

	atomic_inc(&operation->waiters);
	wait_event(gb_operation_cancellation_queue,
			!gb_operation_is_active(operation));
	atomic_dec(&operation->waiters);
}

/**
 * gb_operation_sync: implement a "simple" synchronous gb operation.
 * @connection: the Greybus connection to send this to
 * @type: the type of operation to send
 * @request: pointer to a memory buffer to copy the request from
 * @request_size: size of @request
 * @response: pointer to a memory buffer to copy the response to
 * @response_size: the size of @response.
 * @timeout: operation timeout in milliseconds
 *
 * This function implements a simple synchronous Greybus operation.  It sends
 * the provided operation request and waits (sleeps) until the corresponding
 * operation response message has been successfully received, or an error
 * occurs.  @request and @response are buffers to hold the request and response
 * data respectively, and if they are not NULL, their size must be specified in
 * @request_size and @response_size.
 *
 * If a response payload is to come back, and @response is not NULL,
 * @response_size number of bytes will be copied into @response if the operation
 * is successful.
 *
 * If there is an error, the response buffer is left alone.
 */
int gb_operation_sync_timeout(struct gb_connection *connection, int type,
				void *request, int request_size,
				void *response, int response_size,
				unsigned int timeout)
{
	struct gb_operation *operation;
	int ret;

	if ((response_size && !response) ||
	    (request_size && !request))
		return -EINVAL;

	operation = gb_operation_create(connection, type,
					request_size, response_size,
					GFP_KERNEL);
	if (!operation)
		return -ENOMEM;

	if (request_size)
		memcpy(operation->request->payload, request, request_size);

	ret = gb_operation_request_send_sync_timeout(operation, timeout);
	if (ret) {
		dev_err(&connection->dev, "synchronous operation failed: 0x%02hhx (%d)\n",
			type, ret);
	} else {
		if (response_size) {
			memcpy(response, operation->response->payload,
			       response_size);
		}
	}
	gb_operation_destroy(operation);

	return ret;
}
EXPORT_SYMBOL_GPL(gb_operation_sync_timeout);

int __init gb_operation_init(void)
{
	gb_message_cache = kmem_cache_create("gb_message_cache",
				sizeof(struct gb_message), 0, 0, NULL);
	if (!gb_message_cache)
		return -ENOMEM;

	gb_operation_cache = kmem_cache_create("gb_operation_cache",
				sizeof(struct gb_operation), 0, 0, NULL);
	if (!gb_operation_cache)
		goto err_destroy_message_cache;

	gb_operation_completion_wq = alloc_workqueue("greybus_completion",
				0, 0);
	if (!gb_operation_completion_wq)
		goto err_destroy_operation_cache;

	return 0;

err_destroy_operation_cache:
	kmem_cache_destroy(gb_operation_cache);
	gb_operation_cache = NULL;
err_destroy_message_cache:
	kmem_cache_destroy(gb_message_cache);
	gb_message_cache = NULL;

	return -ENOMEM;
}

void gb_operation_exit(void)
{
	destroy_workqueue(gb_operation_completion_wq);
	gb_operation_completion_wq = NULL;
	kmem_cache_destroy(gb_operation_cache);
	gb_operation_cache = NULL;
	kmem_cache_destroy(gb_message_cache);
	gb_message_cache = NULL;
}
