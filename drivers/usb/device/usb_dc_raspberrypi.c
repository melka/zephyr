/*
 * Copyright (c) 2021, Pete Johanson
 * Copyright (c) 2017 Christer Weinigel.
 * Copyright (c) 2017, I-SENSE group of ICCS
 *
 * SPDX-License-Identifier: Apache-2.0
 */


#include <soc.h>
#include <string.h>
#include <usb/usb_device.h>
#include <sys/util.h>
#include <hardware/regs/usb.h>
#include <hardware/structs/usb.h>
#include <hardware/resets.h>

#define LOG_LEVEL CONFIG_USB_DRIVER_LOG_LEVEL
#include <logging/log.h>
LOG_MODULE_REGISTER(usb_dc_raspberrypi);

#define DT_DRV_COMPAT raspberrypi_rp2_usbd

#define USB_BASE_ADDRESS	DT_INST_REG_ADDR(0)
#define USB_IRQ			DT_INST_IRQ_BY_NAME(0, usbctrl, irq)
#define USB_IRQ_PRI		DT_INST_IRQ_BY_NAME(0, usbctrl, priority)
#define USB_NUM_BIDIR_ENDPOINTS	DT_INST_PROP(0, num_bidir_endpoints)
/* Size of a USB SETUP packet */
#define SETUP_SIZE 8

/* Helper macros to make it easier to work with endpoint numbers */
#define EP0_IDX 0
#define EP0_IN (EP0_IDX | USB_EP_DIR_IN)
#define EP0_OUT (EP0_IDX | USB_EP_DIR_OUT)

#define EP_MPS 64U

#define DATA_BUFFER_SIZE 64U

// Needed for pico-sdk
#ifndef typeof
#define typeof  __typeof__
#endif

#define usb_hw_set hw_set_alias(usb_hw)
#define usb_hw_clear hw_clear_alias(usb_hw)

/* Endpoint state */
struct usb_dc_raspberrypi_ep_state {
	uint16_t ep_mps;		      /** Endpoint max packet size */
	enum usb_dc_ep_transfer_type ep_type; /** Endpoint type */
	uint8_t ep_stalled;		      /** Endpoint stall flag */
	usb_dc_ep_callback cb;		      /** Endpoint callback function */
	uint32_t read_offset;		      /** Current offset in read buffer */
	struct k_sem write_sem;		      /** Write boolean semaphore */
	io_rw_32 *endpoint_control;
	io_rw_32 *buffer_control;
	uint8_t *data_buffer;
	uint8_t next_pid;
};

/* Driver state */
struct usb_dc_raspberrypi_state {
	usb_dc_status_callback status_cb; /* Status callback */
	struct usb_dc_raspberrypi_ep_state out_ep_state[USB_NUM_BIDIR_ENDPOINTS];
	struct usb_dc_raspberrypi_ep_state in_ep_state[USB_NUM_BIDIR_ENDPOINTS];
	// uint8_t ep_buf[USB_NUM_BIDIR_ENDPOINTS][USB_MAX_PACKET_SIZE];
};

static struct usb_dc_raspberrypi_state usb_dc_raspberrypi_state;

/* Internal functions */

static struct usb_dc_raspberrypi_ep_state *usb_dc_raspberrypi_get_ep_state(uint8_t ep)
{
	struct usb_dc_raspberrypi_ep_state *ep_state_base;

	if (USB_EP_GET_IDX(ep) >= USB_NUM_BIDIR_ENDPOINTS) {
		return NULL;
	}

	if (USB_EP_DIR_IS_OUT(ep)) {
		ep_state_base = usb_dc_raspberrypi_state.out_ep_state;
	} else {
		ep_state_base = usb_dc_raspberrypi_state.in_ep_state;
	}

	return ep_state_base + USB_EP_GET_IDX(ep);
}

void usb_dc_raspberrypi_handle_setup()
{
	struct usb_dc_raspberrypi_ep_state *ep = usb_dc_raspberrypi_get_ep_state(EP0_OUT);
	ep->cb(EP0_OUT, USB_DC_EP_SETUP);
}

void usb_dc_raspberrypi_handle_buff_status()
{
	struct usb_dc_raspberrypi_ep_state *ep;
	enum usb_dc_ep_cb_status_code status_code;
	uint8_t status = usb_hw->buf_status;
	unsigned int i;
	unsigned int bit = 1U;

	for (i = 0U; status && i < USB_NUM_BIDIR_ENDPOINTS * 2; i++) {
		if (status & bit) {
			hw_clear_alias(usb_hw)->buf_status = bit;
			bool in = !(bit & 1U);
			uint8_t ep_addr = (i >> 1U) | (in ? USB_EP_DIR_IN : USB_EP_DIR_OUT);
			ep = usb_dc_raspberrypi_get_ep_state(ep_addr);
			status_code = in ? USB_DC_EP_DATA_IN : USB_DC_EP_DATA_OUT; 

			ep->cb(ep_addr, status_code);

			status &= ~bit;
		}

		bit <<= 1U;
	}
}

static void usb_dc_raspberrypi_isr(const void *arg)
{
	// USB interrupt handler
	uint32_t status = usb_hw->ints;
	uint32_t handled = 0;

	// Setup packet received
	if (status & USB_INTS_SETUP_REQ_BITS) {
		handled |= USB_INTS_SETUP_REQ_BITS;
		usb_hw->sie_status = USB_SIE_STATUS_SETUP_REC_BITS;

		usb_dc_raspberrypi_handle_setup();
	}

	// Buffer status, one or more buffers have completed
	if (status & USB_INTS_BUFF_STATUS_BITS) {
		handled |= USB_INTS_BUFF_STATUS_BITS;
		usb_dc_raspberrypi_handle_buff_status();
	}

	// Connection status update
	if (status & USB_INTS_DEV_CONN_DIS_BITS) {
		handled |= USB_INTS_DEV_CONN_DIS_BITS;
		usb_dc_raspberrypi_state.status_cb(usb_hw->sie_status & USB_SIE_STATUS_CONNECTED_BITS ? USB_DC_CONNECTED : USB_DC_DISCONNECTED, NULL);
	}

	// Bus is reset
	if (status & USB_INTS_BUS_RESET_BITS) {
		LOG_WRN("BUS RESET");
		handled |= USB_INTS_BUS_RESET_BITS;
		usb_hw->sie_status = USB_SIE_STATUS_BUS_RESET_BITS;
		usb_dc_raspberrypi_state.status_cb(USB_DC_RESET, NULL);
	}
}

void usb_dc_raspberrypi_init_bidir_endpoint(uint8_t i)
{
	usb_dc_raspberrypi_state.out_ep_state[i].buffer_control = &usb_dpram->ep_buf_ctrl[i].out;
	usb_dc_raspberrypi_state.in_ep_state[i].buffer_control = &usb_dpram->ep_buf_ctrl[i].in;

	if (i != EP0_IDX) {
		usb_dc_raspberrypi_state.out_ep_state[i].endpoint_control = &usb_dpram->ep_ctrl[i].out;
		usb_dc_raspberrypi_state.in_ep_state[i].endpoint_control = &usb_dpram->ep_ctrl[i].in;

		usb_dc_raspberrypi_state.out_ep_state[i].data_buffer = &usb_dpram->epx_data[((i - 1) * 2 + 1) * DATA_BUFFER_SIZE];
		usb_dc_raspberrypi_state.in_ep_state[i].data_buffer = &usb_dpram->epx_data[((i - 1) * 2) * DATA_BUFFER_SIZE];
	} else {
		usb_dc_raspberrypi_state.out_ep_state[i].data_buffer = &usb_dpram->ep0_buf_a[0];
		usb_dc_raspberrypi_state.in_ep_state[i].data_buffer = &usb_dpram->ep0_buf_a[0];

	}

	k_sem_init(&usb_dc_raspberrypi_state.in_ep_state[i].write_sem, 1, 1);

}

static int usb_dc_raspberrypi_init(void)
{
	unsigned int i;

	// Reset usb controller
	reset_block(RESETS_RESET_USBCTRL_BITS);
	unreset_block_wait(RESETS_RESET_USBCTRL_BITS);
	
	// Clear any previous state in dpram just in case
	memset(usb_dpram, 0, sizeof(*usb_dpram)); // <1>
	
	// Mux the controller to the onboard usb phy
	usb_hw->muxing = USB_USB_MUXING_TO_PHY_BITS | USB_USB_MUXING_SOFTCON_BITS;
	
	// Force VBUS detect so the device thinks it is plugged into a host
	usb_hw->pwr = USB_USB_PWR_VBUS_DETECT_BITS | USB_USB_PWR_VBUS_DETECT_OVERRIDE_EN_BITS;
	
	// Enable the USB controller in device mode.
	usb_hw->main_ctrl = USB_MAIN_CTRL_CONTROLLER_EN_BITS;
	
	// Enable an interrupt per EP0 transaction
	usb_hw->sie_ctrl = USB_SIE_CTRL_EP0_INT_1BUF_BITS; // <2>
	
	// Enable interrupts for when a buffer is done, when the bus is reset,
	// and when a setup packet is received, and device connection status
	usb_hw->inte = USB_INTS_BUFF_STATUS_BITS |
	               USB_INTS_BUS_RESET_BITS |
	               USB_INTS_DEV_CONN_DIS_BITS |
	               USB_INTS_SETUP_REQ_BITS;
	
	// Set up endpoints (endpoint control registers)
	// described by device configuration
	// usb_setup_endpoints();
	for (i = 0U; i < USB_NUM_BIDIR_ENDPOINTS; i++) {
		usb_dc_raspberrypi_init_bidir_endpoint(i);
	}
	
	IRQ_CONNECT(USB_IRQ, USB_IRQ_PRI,
		    usb_dc_raspberrypi_isr, 0, 0);
	irq_enable(USB_IRQ);

	// Present full speed device by enabling pull up on DP
	usb_hw->sie_ctrl = USB_SIE_CTRL_PULLUP_EN_BITS;

	return 0;
}

/* Zephyr USB device controller API implementation */

int usb_dc_attach(void)
{
	int ret;

	LOG_DBG("");

	ret = usb_dc_raspberrypi_init();
	if (ret) {
		return ret;
	}

	return 0;
}

int usb_dc_ep_set_callback(const uint8_t ep, const usb_dc_ep_callback cb)
{
	struct usb_dc_raspberrypi_ep_state *ep_state = usb_dc_raspberrypi_get_ep_state(ep);

	LOG_DBG("ep 0x%02x", ep);

	if (!ep_state) {
		return -EINVAL;
	}

	ep_state->cb = cb;

	return 0;
}

void usb_dc_set_status_callback(const usb_dc_status_callback cb)
{
	LOG_DBG("");

	usb_dc_raspberrypi_state.status_cb = cb;
}

int usb_dc_set_address(const uint8_t addr)
{
	LOG_DBG("addr %u (0x%02x)", addr, addr);


	return -ENOTSUP;
}

int usb_dc_ep_start_read(uint8_t ep)
{
	LOG_DBG("ep 0x%02x", ep);

#if 0
	/* we flush EP0_IN by doing a 0 length receive on it */
	if (!USB_EP_DIR_IS_OUT(ep) && (ep != EP0_IN || max_data_len)) {
		LOG_ERR("invalid ep 0x%02x", ep);
		return -EINVAL;
	}

	if (max_data_len > EP_MPS) {
		max_data_len = EP_MPS;
	}

	status = HAL_PCD_EP_Receive(&usb_dc_raspberrypi_state.pcd, ep,
				    usb_dc_raspberrypi_state.ep_buf[USB_EP_GET_IDX(ep)],
				    max_data_len);
	if (status != HAL_OK) {
		LOG_ERR("HAL_PCD_EP_Receive failed(0x%02x), %d", ep,
			(int)status);
		return -EIO;
	}
#endif

	return 0;
}

int usb_dc_ep_check_cap(const struct usb_dc_ep_cfg_data * const cfg)
{
	uint8_t ep_idx = USB_EP_GET_IDX(cfg->ep_addr);

	LOG_DBG("ep %x, mps %d, type %d", cfg->ep_addr, cfg->ep_mps,
		cfg->ep_type);

	if ((cfg->ep_type == USB_DC_EP_CONTROL) && ep_idx) {
		LOG_ERR("invalid endpoint configuration");
		return -1;
	}

	if (ep_idx > (USB_NUM_BIDIR_ENDPOINTS - 1)) {
		LOG_ERR("endpoint index/address out of range");
		return -1;
	}

	return 0;
}

int usb_dc_ep_configure(const struct usb_dc_ep_cfg_data * const ep_cfg)
{
	uint8_t ep = ep_cfg->ep_addr;
	struct usb_dc_raspberrypi_ep_state *ep_state = usb_dc_raspberrypi_get_ep_state(ep);

	if (!ep_state) {
		return -EINVAL;
	}

	LOG_DBG("ep 0x%02x, previous ep_mps %u, ep_mps %u, ep_type %u",
		ep_cfg->ep_addr, ep_state->ep_mps, ep_cfg->ep_mps,
		ep_cfg->ep_type);

	ep_state->ep_mps = ep_cfg->ep_mps;
	ep_state->ep_type = ep_cfg->ep_type;

	return 0;
}

int usb_dc_ep_set_stall(const uint8_t ep)
{
	struct usb_dc_raspberrypi_ep_state *ep_state = usb_dc_raspberrypi_get_ep_state(ep);
	uint8_t val;

	LOG_DBG("ep 0x%02x", ep);

	if (!ep_state) {
		return -EINVAL;
	}
	
	if (!ep_state->endpoint_control) {
		usb_hw_set->ep_stall_arm = USB_EP_DIR_IS_OUT(ep) ? USB_EP_STALL_ARM_EP0_OUT_BITS : USB_EP_STALL_ARM_EP0_IN_BITS ;
	} else {
		val = *ep_state->endpoint_control;
		val |= USB_BUF_CTRL_STALL;

		*ep_state->endpoint_control = val;
	}

	ep_state->ep_stalled = 1U;

	return 0;
}

int usb_dc_ep_clear_stall(const uint8_t ep)
{
	struct usb_dc_raspberrypi_ep_state *ep_state = usb_dc_raspberrypi_get_ep_state(ep);
	uint8_t val;

	LOG_DBG("ep 0x%02x", ep);

	if (!ep_state) {
		return -EINVAL;
	}

	if (!ep_state->endpoint_control) {
		usb_hw_clear->ep_stall_arm = USB_EP_DIR_IS_OUT(ep) ? USB_EP_STALL_ARM_EP0_OUT_BITS : USB_EP_STALL_ARM_EP0_IN_BITS ;
	} else {
		val = *ep_state->endpoint_control;
		val &= ~USB_BUF_CTRL_STALL;

		*ep_state->endpoint_control = val;
	}

	ep_state->ep_stalled = 0U;
	ep_state->read_offset = 0U;

	return 0;
}

int usb_dc_ep_is_stalled(const uint8_t ep, uint8_t *const stalled)
{
	struct usb_dc_raspberrypi_ep_state *ep_state = usb_dc_raspberrypi_get_ep_state(ep);

	LOG_DBG("ep 0x%02x", ep);

	if (!ep_state || !stalled) {
		return -EINVAL;
	}

	*stalled = ep_state->ep_stalled;

	return 0;
}

static inline uint32_t usb_dc_ep_raspberrypi_buffer_offset(volatile uint8_t *data_buffer)
{
	return (uint32_t) data_buffer ^ (uint32_t) usb_dpram;
}

int usb_dc_ep_enable(const uint8_t ep)
{
	struct usb_dc_raspberrypi_ep_state *ep_state = usb_dc_raspberrypi_get_ep_state(ep);

	LOG_DBG("ep 0x%02x", ep);

	if (!ep_state) {
		return -EINVAL;
	}

	// EP0 doesn't have an endpoint_control
	if (!ep_state->endpoint_control) {
		return 0;
	}

	uint32_t val = EP_CTRL_ENABLE_BITS
		| EP_CTRL_INTERRUPT_PER_BUFFER
		| (ep_state->ep_type << EP_CTRL_BUFFER_TYPE_LSB)
		| usb_dc_ep_raspberrypi_buffer_offset(ep_state->data_buffer);

	*ep_state->endpoint_control = val;

	if (USB_EP_DIR_IS_OUT(ep) && ep != EP0_OUT) {
		return usb_dc_ep_start_read(ep);
	}

	return 0;
}

int usb_dc_ep_disable(const uint8_t ep)
{
	struct usb_dc_raspberrypi_ep_state *ep_state = usb_dc_raspberrypi_get_ep_state(ep);

	LOG_DBG("ep 0x%02x", ep);

	if (!ep_state) {
		return -EINVAL;
	}
	
	// EP0 doesn't have an endpoint_control
	if (!ep_state->endpoint_control) {
		return 0;
	}

	uint8_t val = *ep_state->endpoint_control;
	val &= ~EP_CTRL_ENABLE_BITS;

	*ep_state->endpoint_control = val;

	return 0;
}

int usb_dc_ep_write(const uint8_t ep, const uint8_t *const data,
		    const uint32_t data_len, uint32_t * const ret_bytes)
{
	struct usb_dc_raspberrypi_ep_state *ep_state = usb_dc_raspberrypi_get_ep_state(ep);
	uint32_t len = data_len;
	int ret = 0;

	LOG_DBG("ep 0x%02x, len %u", ep, data_len);

	if (!ep_state || !USB_EP_DIR_IS_IN(ep)) {
		LOG_ERR("invalid ep 0x%02x", ep);
		return -EINVAL;
	}

	ret = k_sem_take(&ep_state->write_sem, K_NO_WAIT);
	if (ret) {
		LOG_ERR("Unable to get write lock (%d)", ret);
		return -EAGAIN;
	}

	if (!k_is_in_isr()) {
		irq_disable(USB_IRQ);
	}

	if (ep == EP0_IN && len > USB_MAX_CTRL_MPS) {
		len = USB_MAX_CTRL_MPS;
	}

#if 0
	status = HAL_PCD_EP_Transmit(&usb_dc_raspberrypi_state.pcd, ep,
				     (void *)data, len);
	if (status != HAL_OK) {
		LOG_ERR("HAL_PCD_EP_Transmit failed(0x%02x), %d", ep,
			(int)status);
		k_sem_give(&ep_state->write_sem);
		ret = -EIO;
	}
#endif

	if (!ret && ep == EP0_IN && len > 0) {
		/* Wait for an empty package as from the host.
		 * This also flushes the TX FIFO to the host.
		 */
		usb_dc_ep_start_read(ep);
	}

	if (!k_is_in_isr()) {
		irq_enable(USB_IRQ);
	}

	if (!ret && ret_bytes) {
		*ret_bytes = len;
	}

	return ret;
}

uint32_t usb_dc_raspberrypi_get_ep_in_buffer_len(const uint8_t ep)
{
	struct usb_dc_raspberrypi_ep_state *ep_state = usb_dc_raspberrypi_get_ep_state(ep);
	uint32_t buf_ctl = *ep_state->buffer_control;

	return buf_ctl & USB_BUF_CTRL_LEN_MASK;
}

int usb_dc_ep_read_wait(uint8_t ep, uint8_t *data, uint32_t max_data_len,
			uint32_t *read_bytes)
{
	struct usb_dc_raspberrypi_ep_state *ep_state = usb_dc_raspberrypi_get_ep_state(ep);
	uint32_t read_count;

	if (!ep_state) {
		LOG_ERR("Invalid Endpoint %x", ep);
		return -EINVAL;
	}

	read_count = usb_dc_raspberrypi_get_ep_in_buffer_len(ep) - ep_state->read_offset;

	LOG_DBG("ep 0x%02x, %u bytes, %u+%u, %p", ep, max_data_len,
		ep_state->read_offset, read_count, data);

	if (!USB_EP_DIR_IS_OUT(ep)) { /* check if OUT ep */
		LOG_ERR("Wrong endpoint direction: 0x%02x", ep);
		return -EINVAL;
	}

	if (data) {
		read_count = MIN(read_count, max_data_len);
		memcpy(data, ep_state->data_buffer +
		       ep_state->read_offset, read_count);
		ep_state->read_offset += read_count;
	} else if (max_data_len) {
		LOG_ERR("Wrong arguments");
	}

	if (read_bytes) {
		*read_bytes = read_count;
	}

	return 0;
}

int usb_dc_ep_read_continue(uint8_t ep)

{
	struct usb_dc_raspberrypi_ep_state *ep_state = usb_dc_raspberrypi_get_ep_state(ep);

	if (!ep_state || !USB_EP_DIR_IS_OUT(ep)) { /* Check if OUT ep */
		LOG_ERR("Not valid endpoint: %02x", ep);
		return -EINVAL;
	}

	/* If no more data in the buffer, start a new read transaction.
	 */
	if (usb_dc_raspberrypi_get_ep_in_buffer_len(ep) == ep_state->read_offset) {
		LOG_DBG("Start a new read!");
		return -ENOTSUP;
	}

	return 0;
}

int usb_dc_ep_read(const uint8_t ep, uint8_t *const data, const uint32_t max_data_len,
		   uint32_t * const read_bytes)
{
	if (usb_dc_ep_read_wait(ep, data, max_data_len, read_bytes) != 0) {
		return -EINVAL;
	}

	if (usb_dc_ep_read_continue(ep) != 0) {
		return -EINVAL;
	}

	return 0;
}

int usb_dc_ep_halt(const uint8_t ep)
{
	return usb_dc_ep_set_stall(ep);
}

int usb_dc_ep_flush(const uint8_t ep)
{
	struct usb_dc_raspberrypi_ep_state *ep_state = usb_dc_raspberrypi_get_ep_state(ep);

	if (!ep_state) {
		return -EINVAL;
	}

	LOG_ERR("Not implemented");

	return 0;
}

int usb_dc_ep_mps(const uint8_t ep)
{
	struct usb_dc_raspberrypi_ep_state *ep_state = usb_dc_raspberrypi_get_ep_state(ep);

	if (!ep_state) {
		return -EINVAL;
	}

	return ep_state->ep_mps;
}

int usb_dc_detach(void)
{
	LOG_ERR("Not implemented");

	return 0;
}

int usb_dc_reset(void)
{
	LOG_ERR("Not implemented");

	return 0;
}

/* Callbacks from the STM32 Cube HAL code */

#if 0
void HAL_PCD_ResetCallback(PCD_HandleTypeDef *hpcd)
{
	int i;

	LOG_DBG("");

	HAL_PCD_EP_Open(&usb_dc_stm32_state.pcd, EP0_IN, EP0_MPS, EP_TYPE_CTRL);
	HAL_PCD_EP_Open(&usb_dc_stm32_state.pcd, EP0_OUT, EP0_MPS,
			EP_TYPE_CTRL);

	/* The DataInCallback will never be called at this point for any pending
	 * transactions. Reset the IN semaphores to prevent perpetual locked state.
	 * */
	for (i = 0; i < USB_NUM_BIDIR_ENDPOINTS; i++) {
		k_sem_give(&usb_dc_stm32_state.in_ep_state[i].write_sem);
	}

	if (usb_dc_stm32_state.status_cb) {
		usb_dc_stm32_state.status_cb(USB_DC_RESET, NULL);
	}
}

void HAL_PCD_ConnectCallback(PCD_HandleTypeDef *hpcd)
{
	LOG_DBG("");

	if (usb_dc_stm32_state.status_cb) {
		usb_dc_stm32_state.status_cb(USB_DC_CONNECTED, NULL);
	}
}

void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef *hpcd)
{
	LOG_DBG("");

	if (usb_dc_stm32_state.status_cb) {
		usb_dc_stm32_state.status_cb(USB_DC_DISCONNECTED, NULL);
	}
}

void HAL_PCD_SuspendCallback(PCD_HandleTypeDef *hpcd)
{
	LOG_DBG("");

	if (usb_dc_stm32_state.status_cb) {
		usb_dc_stm32_state.status_cb(USB_DC_SUSPEND, NULL);
	}
}

void HAL_PCD_ResumeCallback(PCD_HandleTypeDef *hpcd)
{
	LOG_DBG("");

	if (usb_dc_stm32_state.status_cb) {
		usb_dc_stm32_state.status_cb(USB_DC_RESUME, NULL);
	}
}

void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef *hpcd)
{
	struct usb_setup_packet *setup = (void *)usb_dc_stm32_state.pcd.Setup;
	struct usb_dc_stm32_ep_state *ep_state;

	LOG_DBG("");

	ep_state = usb_dc_stm32_get_ep_state(EP0_OUT); /* can't fail for ep0 */
	__ASSERT(ep_state, "No corresponding ep_state for EP0");

	ep_state->read_count = SETUP_SIZE;
	ep_state->read_offset = 0U;
	memcpy(&usb_dc_stm32_state.ep_buf[EP0_IDX],
	       usb_dc_stm32_state.pcd.Setup, ep_state->read_count);

	if (ep_state->cb) {
		ep_state->cb(EP0_OUT, USB_DC_EP_SETUP);

		if (!(setup->wLength == 0U) &&
		    !(REQTYPE_GET_DIR(setup->bmRequestType) ==
		    REQTYPE_DIR_TO_HOST)) {
			usb_dc_ep_start_read(EP0_OUT,
					     usb_dc_stm32_state.ep_buf[EP0_IDX],
					     setup->wLength);
		}
	}
}

void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
	uint8_t ep_idx = USB_EP_GET_IDX(epnum);
	uint8_t ep = ep_idx | USB_EP_DIR_OUT;
	struct usb_dc_stm32_ep_state *ep_state = usb_dc_stm32_get_ep_state(ep);

	LOG_DBG("epnum 0x%02x, rx_count %u", epnum,
		HAL_PCD_EP_GetRxCount(&usb_dc_stm32_state.pcd, epnum));

	/* Transaction complete, data is now stored in the buffer and ready
	 * for the upper stack (usb_dc_ep_read to retrieve).
	 */
	usb_dc_ep_get_read_count(ep, &ep_state->read_count);
	ep_state->read_offset = 0U;

	if (ep_state->cb) {
		ep_state->cb(ep, USB_DC_EP_DATA_OUT);
	}
}

void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
	uint8_t ep_idx = USB_EP_GET_IDX(epnum);
	uint8_t ep = ep_idx | USB_EP_DIR_IN;
	struct usb_dc_stm32_ep_state *ep_state = usb_dc_stm32_get_ep_state(ep);

	LOG_DBG("epnum 0x%02x", epnum);

	__ASSERT(ep_state, "No corresponding ep_state for ep");

	k_sem_give(&ep_state->write_sem);

	if (ep_state->cb) {
		ep_state->cb(ep, USB_DC_EP_DATA_IN);
	}
}

#if defined(USB) && defined(CONFIG_USB_DC_STM32_DISCONN_ENABLE)
void HAL_PCDEx_SetConnectionState(PCD_HandleTypeDef *hpcd, uint8_t state)
{
	const struct device *usb_disconnect;

	usb_disconnect = device_get_binding(
				DT_GPIO_LABEL(DT_INST(0, st_stm32_usb), disconnect_gpios));

	gpio_pin_configure(usb_disconnect,
			   DT_GPIO_PIN(DT_INST(0, st_stm32_usb), disconnect_gpios),
			   DT_GPIO_FLAGS(DT_INST(0, st_stm32_usb), disconnect_gpios) |
			   (state ? GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE));
}
#endif /* USB && CONFIG_USB_DC_STM32_DISCONN_ENABLE */

#endif
