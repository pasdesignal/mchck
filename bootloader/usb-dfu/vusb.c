#include <sys/socket.h>
#include <arpa/inet.h>

#include <err.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <poll.h>

#include "usb.h"


/* From usbip_common.h */


/*
 * USB/IP request headers
 *
 * Each request is transferred across the network to its counterpart, which
 * facilitates the normal USB communication. The values contained in the headers
 * are basically the same as in a URB. Currently, four request types are
 * defined:
 *
 *  - USBIP_CMD_SUBMIT: a USB request block, corresponds to usb_submit_urb()
 *    (client to server)
 *
 *  - USBIP_RET_SUBMIT: the result of USBIP_CMD_SUBMIT
 *    (server to client)
 *
 *  - USBIP_CMD_UNLINK: an unlink request of a pending USBIP_CMD_SUBMIT,
 *    corresponds to usb_unlink_urb()
 *    (client to server)
 *
 *  - USBIP_RET_UNLINK: the result of USBIP_CMD_UNLINK
 *    (server to client)
 *
 */
#define USBIP_CMD_SUBMIT	0x0001
#define USBIP_CMD_UNLINK	0x0002
#define USBIP_RET_SUBMIT	0x0003
#define USBIP_RET_UNLINK	0x0004

#define USBIP_DIR_OUT	0x00
#define USBIP_DIR_IN	0x01

/**
 * struct usbip_header_basic - data pertinent to every request
 * @command: the usbip request type
 * @seqnum: sequential number that identifies requests; incremented per
 *	    connection
 * @devid: specifies a remote USB device uniquely instead of busnum and devnum;
 *	   in the stub driver, this value is ((busnum << 16) | devnum)
 * @direction: direction of the transfer
 * @ep: endpoint number
 */
struct usbip_header_basic {
	uint32_t command;
	uint32_t seqnum;
	uint32_t devid;
	uint32_t direction;
	uint32_t ep;
} __packed;

/* Intermission, from: linux/usb.h */
/*
 * urb->transfer_flags:
 *
 * Note: URB_DIR_IN/OUT is automatically set in usb_submit_urb().
 */
#define URB_SHORT_NOT_OK	0x0001	/* report short reads as errors */
#define URB_ISO_ASAP		0x0002	/* iso-only; use the first unexpired
					 * slot in the schedule */
#define URB_NO_TRANSFER_DMA_MAP	0x0004	/* urb->transfer_dma valid on submit */
#define URB_NO_FSBR		0x0020	/* UHCI-specific */
#define URB_ZERO_PACKET		0x0040	/* Finish bulk OUT with short packet */
#define URB_NO_INTERRUPT	0x0080	/* HINT: no non-error interrupt
					 * needed */
#define URB_FREE_BUFFER		0x0100	/* Free transfer buffer with the URB */

/* The following flags are used internally by usbcore and HCDs */
#define URB_DIR_IN		0x0200	/* Transfer from device to host */
#define URB_DIR_OUT		0
#define URB_DIR_MASK		URB_DIR_IN

#define URB_DMA_MAP_SINGLE	0x00010000	/* Non-scatter-gather mapping */
#define URB_DMA_MAP_PAGE	0x00020000	/* HCD-unsupported S-G */
#define URB_DMA_MAP_SG		0x00040000	/* HCD-supported S-G */
#define URB_MAP_LOCAL		0x00080000	/* HCD-local-memory mapping */
#define URB_SETUP_MAP_SINGLE	0x00100000	/* Setup packet DMA mapped */
#define URB_SETUP_MAP_LOCAL	0x00200000	/* HCD-local setup packet */
#define URB_DMA_SG_COMBINED	0x00400000	/* S-G entries were combined */
#define URB_ALIGNED_TEMP_BUFFER	0x00800000	/* Temp buffer was alloc'd */

/**
 * struct usbip_header_cmd_submit - USBIP_CMD_SUBMIT packet header
 * @transfer_flags: URB flags
 * @transfer_buffer_length: the data size for (in) or (out) transfer
 * @start_frame: initial frame for isochronous or interrupt transfers
 * @number_of_packets: number of isochronous packets
 * @interval: maximum time for the request on the server-side host controller
 * @setup: setup data for a control request
 */
struct usbip_header_cmd_submit {
	uint32_t transfer_flags;
	int32_t transfer_buffer_length;

	/* it is difficult for usbip to sync frames (reserved only?) */
	int32_t start_frame;
	int32_t number_of_packets;
	int32_t interval;

	unsigned char setup[8];
} __packed;

/**
 * struct usbip_header_ret_submit - USBIP_RET_SUBMIT packet header
 * @status: return status of a non-iso request
 * @actual_length: number of bytes transferred
 * @start_frame: initial frame for isochronous or interrupt transfers
 * @number_of_packets: number of isochronous packets
 * @error_count: number of errors for isochronous transfers
 */
struct usbip_header_ret_submit {
	int32_t status;
	int32_t actual_length;
	int32_t start_frame;
	int32_t number_of_packets;
	int32_t error_count;
} __packed;

/**
 * struct usbip_header_cmd_unlink - USBIP_CMD_UNLINK packet header
 * @seqnum: the URB seqnum to unlink
 */
struct usbip_header_cmd_unlink {
	uint32_t seqnum;
} __packed;

/**
 * struct usbip_header_ret_unlink - USBIP_RET_UNLINK packet header
 * @status: return status of the request
 */
struct usbip_header_ret_unlink {
	int32_t status;
} __packed;

/**
 * struct usbip_header - common header for all usbip packets
 * @base: the basic header
 * @u: packet type dependent header
 */
struct usbip_header {
	struct usbip_header_basic base;

	union {
		struct usbip_header_cmd_submit	cmd_submit;
		struct usbip_header_ret_submit	ret_submit;
		struct usbip_header_cmd_unlink	cmd_unlink;
		struct usbip_header_ret_unlink	ret_unlink;
	} u;
} __packed;

/*
 * This is the same as usb_iso_packet_descriptor but packed for pdu.
 */
struct usbip_iso_packet_descriptor {
	uint32_t offset;
	uint32_t length;			/* expected length */
	uint32_t actual_length;
	uint32_t status;
} __packed;



struct usb_xfer_info {
        enum usb_tok_pid pid;
        void *data;
};

struct vusb_pipe_state_t {
        char *addr;
        size_t len;
        int ready;
        enum usb_data01 data01;

        size_t recv_len;
        enum usb_tok_pid tok;
};

struct vusb_dev_t {
        int address;
        int running;
        int activity;
        struct vusb_ep_state_t {
                int stalled;
                int pingpong_rx;
                int pingpong_tx;
                struct vusb_pipe_state_t rx[2];
                struct vusb_pipe_state_t tx[2];
        } ep[16];
};

struct vusb_urb_t {
        // linked list
        struct vusb_urb_t *next;

        // header
        int seqnum;
        int devid;
        int direction;
        int ep;
        // urb data
        int transfer_flags;
        int transfer_length;
        int start_frame;
        int number_of_packets;
        int interval;
        char setup[8];
        // reply
        int status;
        int actual_length;
        int error_count;

        enum vusb_setup_state {
                VUSB_SETUP_NONE,
                VUSB_SETUP_SETUP,
                VUSB_SETUP_DATA,
                VUSB_SETUP_STATUS,
                VUSB_SETUP_DONE,
        } setup_state;

        // data
        char data[];
};


static int vusb_sock;
static struct vusb_urb_t *urbs;
static struct vusb_dev_t vusb_dev;

static void
vusb_attach(void)
{
        /**
         * Interfacing to usbip-host/vhci_hcd:
         *
         * 1. find free roothub port
         * 2. create socket to pass to the kernel
         * 3. attach
         */

        /* (1) find free roothub port */
        FILE *statusf = fopen("/sys/devices/platform/vhci_hcd/status", "r");

        if (statusf == NULL)
                errx(1, "vhci-hcd module not loaded?");

        int nextport = -1;

        /* skip header */
        fscanf(statusf, "%*[^\n]\n");
        while (!feof(statusf)) {
                int portid, status;
                if (fscanf(statusf, "%d %d %*[^\n]\n", &portid, &status) != 2)
                        warnx("odd vhci status line");

                if (status == 4) {
                        nextport = portid;
                        break;
                }
        }
        fclose(statusf);

        if (nextport < 0)
                errx(1, "no free port found");

        /* (2) create socket */
        int sockp[2];
        if (socketpair(PF_UNIX, SOCK_STREAM, 0, sockp) < 0)
                err(1, "cannot create socket");

        /* (3) pass to kernel */
        /* XXX needs root priv */
        FILE *attachf = fopen("/sys/devices/platform/vhci_hcd/attach", "w");
        if (attachf == NULL)
                err(1, "cannot open attach to vhci");
        if (fprintf(attachf, "%u %u %u %u",
                    nextport,
                    sockp[0],
                    nextport + 1,   /* devid */
                    2               /* USB_SPEED_FULL */
                    ) < 0 ||
            fclose(attachf) < 0)
                err(1, "cannot attach to vhci");

        /* belongs to the kernel now */
        close(sockp[0]);

        vusb_sock = sockp[1];
}

static void
sockread(void *bufp, size_t buflen)
{
        char *buf = bufp;

        for (size_t len = 0; len < buflen; ) {
                ssize_t r;
                r = read(vusb_sock, buf + len, buflen - len);
                if (r < 0)
                        err(1, "read");
                len += r;
        }
}


void *
usb_get_xfer_data(struct usb_xfer_info *i)
{
        return (i->data);
}

enum usb_tok_pid
usb_get_xfer_pid(struct usb_xfer_info *i)
{
        return (i->pid);
}

void
usb_enable_xfers(void)
{
        vusb_dev.running = 1;
        vusb_dev.activity = 1;
}

void
usb_set_addr(int addr)
{
        vusb_dev.address = addr;
}

void
usb_ep_stall(int ep)
{
        vusb_dev.ep[ep].stalled = 1;
}

void
usb_clear_transfers(void)
{
}

inline size_t
usb_ep_get_transfer_size(int ep, enum usb_ep_dir dir, enum usb_ep_pingpong pingpong)
{
        struct vusb_ep_state_t *es = &vusb_dev.ep[ep];
        struct vusb_pipe_state_t *ps = &(dir == USB_EP_TX ? es->tx : es->rx)[pingpong];

        return (ps->recv_len);
}

void
usb_tx_queue_next(struct usbd_ep_pipe_state_t *s, void *addr, size_t len)
{
        /* XXX only ep 0 for now */
        struct vusb_pipe_state_t *ps = &vusb_dev.ep[0].tx[s->pingpong];

        ps->addr = addr;
        ps->len = len;
        ps->ready = 1;
        vusb_dev.activity = 1;
}

void
usb_rx_queue_next(struct usbd_ep_pipe_state_t *s, void *addr, size_t len)
{
        /* XXX only ep 0 for now */
        struct vusb_pipe_state_t *ps = &vusb_dev.ep[0].rx[s->pingpong];

        ps->addr = addr;
        ps->len = len;
        ps->ready = 1;
        vusb_dev.activity = 1;
}


static void
vusb_deliver_packet(struct vusb_pipe_state_t *ps)
{
        struct usb_xfer_info stat;

        stat.pid = ps->tok;
        stat.data = ps->addr;
        usb_handle_transaction(&stat);
}

static void
vusb_urb_to_req(struct vusb_urb_t *urb, struct usbip_header_basic *h)
{
        h->command = htonl(USBIP_RET_SUBMIT);
        h->seqnum = htonl(urb->seqnum);
        h->devid = htonl(urb->devid);
        h->direction = htonl(urb->direction);
        h->ep = htonl(urb->ep);
}

static enum usb_tok_pid
vusb_tx_one(int ep, enum usb_tok_pid tok, void *addr, size_t len)
{
        struct vusb_ep_state_t *es = &vusb_dev.ep[ep];

        switch (tok) {
        case USB_PID_SETUP:
                printf("SETUP\n");
                break;
        case USB_PID_OUT:
                printf("OUT\n");
                break;
        default:
                printf("weh\n");
                break;
        }
        for (size_t i = 0; i < len; ++i)
                printf("%02x%s",
                       ((unsigned char *)addr)[i],
                       (i + 1) % 8 ? " " : "\n");
        if (len % 8 != 0)
                printf("\n");

        if (!vusb_dev.running) {
                printf("-> TIMEOUT\n");
                return (USB_PID_TIMEOUT);
        }

        if (ep == 0 && tok == USB_PID_SETUP) {
                es->stalled = 0;
                vusb_dev.running = 0;
                vusb_dev.activity = 1;
        }

        if (es->stalled) {
                printf("-> STALL\n");
                return (USB_PID_STALL);
        }

        struct vusb_pipe_state_t *ps = &es->rx[es->pingpong_rx];
        if (!ps->ready) {
                printf("-> NAK\n");
                return (USB_PID_NAK);
        }

        size_t xlen = len;
        if (xlen > ps->len)
                xlen = ps->len;

        memcpy(ps->addr, addr, xlen);
        ps->recv_len = len;
        ps->tok = tok;
        ps->ready = 0;
        es->pingpong_rx ^= 1;

        printf("-> ACK%s\n",
               xlen < len ? " (short)" : "");

        vusb_deliver_packet(ps);
        vusb_dev.activity = 1;

        return (USB_PID_ACK);
}

static enum usb_tok_pid
vusb_rx_one(int ep, enum usb_tok_pid tok, void *addr, size_t len, size_t *rxlen)
{
        struct vusb_ep_state_t *es = &vusb_dev.ep[ep];

        switch (tok) {
        case USB_PID_IN:
                printf("IN\n");
                break;
        default:
                printf("weh\n");
                break;
        }

        if (!vusb_dev.running) {
                printf("-> TIMEOUT\n");
                return (USB_PID_TIMEOUT);
        }

        if (es->stalled) {
                printf("-> STALL\n");
                return (USB_PID_STALL);
        }

        struct vusb_pipe_state_t *ps = &es->tx[es->pingpong_tx];
        if (!ps->ready) {
                printf("-> NAK\n");
                return (USB_PID_NAK);
        }

        for (size_t i = 0; i < ps->len; ++i)
                printf("%02x%s",
                       ((unsigned char *)ps->addr)[i],
                       (i + 1) % 8 ? " " : "\n");
        if (ps->len % 8 != 0)
                printf("\n");

        printf("-> ACK%s\n",
               len > ps->len ? " (short)" : "");

        size_t xlen = len;
        if (xlen > ps->len)
                xlen = ps->len;

        memcpy(addr, ps->addr, xlen);
        ps->recv_len = len;
        ps->tok = tok;
        ps->ready = 0;
        es->pingpong_tx ^= 1;
        *rxlen = ps->len;

        vusb_deliver_packet(ps);
        vusb_dev.activity = 1;

        return (USB_PID_ACK);
}

static int
vusb_tx(struct vusb_urb_t *urb)
{
        //struct vusb_ep_state_t *es = &vusb_dev.ep[urb->ep];
        size_t ep_len = 64; /* XXX use maxpacketsize */

        size_t thislen = urb->transfer_length - urb->actual_length;
        if (thislen > ep_len)
                thislen = ep_len;

        enum usb_tok_pid tok;
        tok = vusb_tx_one(urb->ep,
                          USB_PID_OUT,
                          urb->data + urb->actual_length,
                          thislen);
        switch (tok) {
        case USB_PID_STALL:
                urb->status = -EPIPE;
                return (-1);
        case USB_PID_ACK:
                urb->actual_length += thislen;
                /* Are we done? */
                if (urb->actual_length == urb->transfer_length &&
                    /* short packet sent or no zero asked for */
                    (thislen < ep_len ||
                     !(urb->transfer_flags & URB_ZERO_PACKET))) {
                        return (1);
                }
        default:
                break;
        }

        return (0);
}

static int
vusb_rx(struct vusb_urb_t *urb)
{
        //struct vusb_ep_state_t *es = &vusb_dev.ep[urb->ep];
        size_t ep_len = 64; /* XXX use maxpacketsize */

        size_t thislen = urb->transfer_length - urb->actual_length;
        if (thislen > ep_len)
                thislen = ep_len;

        size_t rxlen;
        enum usb_tok_pid tok;
        tok = vusb_rx_one(urb->ep,
                          USB_PID_IN,
                          urb->data + urb->actual_length,
                          thislen,
                          &rxlen);
        switch (tok) {
        case USB_PID_STALL:
                urb->status = -EPIPE;
                return (-1);
        case USB_PID_ACK:
                 if (rxlen < thislen &&
                     (urb->transfer_flags & URB_SHORT_NOT_OK)) {
                         urb->status = -EREMOTEIO;
                         return (-1);
                 } else {
                         urb->actual_length += rxlen;
                         /* Are we done? */
                         if (urb->actual_length == urb->transfer_length ||
                             rxlen < thislen) {
                                 return (1);
                         }
                 }
        default:
                break;
        }

        return (0);
}

static int
vusb_setup_status(struct vusb_urb_t *urb)
{
        int direction = !urb->direction;

        if (direction == USBIP_DIR_OUT) {
                switch (vusb_tx_one(urb->ep, USB_PID_OUT, NULL, 0)) {
                case USB_PID_ACK:
                        urb->setup_state = VUSB_SETUP_DONE;
                        return (1);
                case USB_PID_STALL:
                        urb->status = -EPIPE;
                        return (-1);
                default:
                        return (0);
                }
        } else {
                size_t rxlen;

                switch (vusb_rx_one(urb->ep, USB_PID_IN, NULL, 0, &rxlen)) {
                case USB_PID_STALL:
                        urb->status = -EPIPE;
                        return (-1);
                case USB_PID_ACK:
                        if (rxlen > 0)
                                urb->status = -EOVERFLOW;
                        urb->setup_state = VUSB_SETUP_DONE;
                        return (1);
                default:
                        return (0);
                }
        }
        /* NOTREACHED */
}



static int
vusb_process_urb(struct vusb_urb_t *urb)
{
        int direction = urb->direction;
        int status = 0;

        switch (urb->setup_state) {
        case VUSB_SETUP_SETUP:
                if (vusb_tx_one(urb->ep, USB_PID_SETUP, urb->setup, sizeof(urb->setup)) != USB_PID_ACK)
                        return (-1);
                if (urb->transfer_length > 0)
                        urb->setup_state = VUSB_SETUP_DATA;
                else
                        urb->setup_state = VUSB_SETUP_STATUS;
                return (0);

        case VUSB_SETUP_STATUS:
                return (vusb_setup_status(urb));

        case VUSB_SETUP_DATA:
        case VUSB_SETUP_NONE:
        default:
                if (direction == USBIP_DIR_OUT)
                        status = vusb_tx(urb);
                else
                        status = vusb_rx(urb);

                if (status > 0 && urb->setup_state == VUSB_SETUP_DATA) {
                        urb->setup_state = VUSB_SETUP_STATUS;
                        status = 0;
                }

                return (status);
        }

        return (-1);
}

static void
handle_cmd_submit(struct usbip_header *requn)
{
        struct usbip_header_basic *h = &requn->base;
        struct usbip_header_cmd_submit *req = &requn->u.cmd_submit;
        struct vusb_urb_t *urb;
        int len;

        len = ntohl(req->transfer_buffer_length);

        urb = calloc(1, sizeof(*urb) + len);
        if (urb == NULL)
                err(1, "cannot allocate urb");

        urb->seqnum = h->seqnum;
        urb->devid = h->devid;
        urb->direction = h->direction;
        urb->ep = h->ep;

        urb->transfer_flags = ntohl(req->transfer_flags);
        urb->transfer_length = ntohl(req->transfer_buffer_length);
        urb->start_frame = ntohl(req->start_frame);
        urb->number_of_packets = ntohl(req->number_of_packets);
        urb->interval = ntohl(req->interval);

        memcpy(urb->setup, req->setup, sizeof(urb->setup));

        /* unfortunately we have to infer a setup transfer */
        char zeros[8] = {0};
        if (urb->ep == 0 && memcmp(urb->setup, zeros, sizeof(zeros)) != 0) {
                urb->setup_state = VUSB_SETUP_SETUP;
        }

        if (urb->direction == USBIP_DIR_OUT) {
                sockread(urb->data, urb->transfer_length);
        }

        urb->next = urbs;
        urbs = urb;
        vusb_dev.activity = 1;
}

static void
vusb_ret_submit(struct vusb_urb_t *urb)
{
        struct usbip_header requn;
        struct iovec iov[3];
        int iovcnt = 0;
        struct usbip_header_ret_submit *r = &requn.u.ret_submit;

        vusb_urb_to_req(urb, &requn.base);
        r->status = htonl(urb->status);
        r->actual_length = htonl(urb->actual_length);
        r->start_frame = htonl(urb->start_frame);
        r->number_of_packets = htonl(urb->number_of_packets);
        r->error_count = htonl(urb->error_count);

        iov[iovcnt].iov_base = &requn;
        iov[iovcnt].iov_len = sizeof(requn);
        iovcnt++;

        if (urb->direction == USBIP_DIR_IN) {
                iov[iovcnt].iov_base = urb->data;
                iov[iovcnt].iov_len = urb->actual_length;
                iovcnt++;
        }

        /* XXX iso */
        if (writev(vusb_sock, iov, iovcnt) < 0)
                err(1, "send urb return");
}

static void
vusb_process_urbs(void)
{
        struct vusb_urb_t **p;
        struct vusb_urb_t *urb;

        for (p = &urbs; (urb = *p) != NULL; p = *p ? &(*p)->next : p) {
                if (vusb_process_urb(urb)) {
                        vusb_ret_submit(urb);
                        *p = urb->next;
                        free(urb);
                }
        }
}

static void
vusb_rcv(int block)
{
        struct usbip_header requn;
        struct usbip_header_basic *h = &requn.base;

        if (!block) {
                struct pollfd pfd;

                /* check if there is data */
                pfd.fd = vusb_sock;
                pfd.events = POLLIN;
                if (poll(&pfd, 1, 0) <= 0)
                        return;
        }

        sockread(&requn, sizeof(requn));

        h->command = ntohl(h->command);
        h->seqnum = ntohl(h->seqnum);
        h->devid = ntohl(h->devid);
        h->direction = ntohl(h->direction);
        h->ep = ntohl(h->ep);

        printf("command: %u, seqnum: %u, devid: %u, direction: %u, ep: %u\n",
               h->command,
               h->seqnum,
               h->devid,
               h->direction,
               h->ep);

        switch (h->command) {
        case USBIP_CMD_SUBMIT:
                handle_cmd_submit(&requn);
                break;
        }
}

const static struct usb_desc_dev_t dev_desc = {
        .bLength = sizeof(struct usb_desc_dev_t),
        .bDescriptorType = USB_DESC_DEV,
        .bcdUSB = { .maj = 2 },
        .bDeviceClass = USB_DEV_CLASS_SEE_IFACE,
        .bDeviceSubClass = USB_DEV_SUBCLASS_SEE_IFACE,
        .bDeviceProtocol = USB_DEV_PROTO_SEE_IFACE,
        .bMaxPacketSize0 = EP0_BUFSIZE,
        .idVendor = 0x2323,
        .idProduct = 1,
        .bcdDevice = { .bcd = 0 },
        .iManufacturer = 1,
        .iProduct = 1,
        .iSerialNumber = 0,
        .bNumConfigurations = 1,
};

const static struct usb_desc_config_t config_desc = {
        .bLength = sizeof(struct usb_desc_config_t),
        .bDescriptorType = USB_DESC_CONFIG,
        .wTotalLength = sizeof(config_desc),
        .bNumInterfaces = 1,
        .bConfigurationValue = 0,
        .iConfiguration = 0,
        .remote_wakeup = 0,
        .self_powered = 0,
        .one = 1,
        .bMaxPower = 50,
        .iface_descs = {{
                        .bLength = sizeof(struct usb_desc_iface_t),
                        .bDescriptorType = USB_DESC_IFACE,
                        .bInterfaceNumber = 0,
                        .bAlternateSetting = 0,
                        .bNumEndpoints = 0,
                        .bInterfaceClass = USB_IFACE_CLASS_VENDOR,
                        .bInterfaceSubClass = 0,
                        .bInterfaceProtocol = 0,
                        .iInterface = 0,
                }},
};

const static struct usb_desc_string_t * const string_descs[] = {
#define USB_DEF_DESC_STRING(s)                  \
        (void *)&(struct {                                              \
        struct usb_desc_string_t dsc;                                   \
        char16_t str[sizeof(s) / 2 - 1];                                 \
        } __packed) {{                                                  \
                .bLength = sizeof(struct usb_desc_string_t) + sizeof(s) - 2, \
                .bDescriptorType = USB_DESC_STRING,                     \
                },                                                      \
           s                                                            \
        }
        USB_DEF_DESC_STRING(u"\x0409"),
        USB_DEF_DESC_STRING(u"MCHCK test"),
        NULL
};

int
main(void)
{
        usb_start(&dev_desc, &config_desc, string_descs);
        vusb_attach();
        for (;;) {
                vusb_rcv(urbs == NULL || vusb_dev.activity == 0);
                vusb_dev.activity = 0;
                vusb_process_urbs();
        }
}
