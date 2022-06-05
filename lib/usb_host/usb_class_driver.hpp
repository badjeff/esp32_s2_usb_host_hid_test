/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "stdlib.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "usb/usb_host.h"

#define CLIENT_NUM_EVENT_MSG        5

#define ACTION_OPEN_DEV             0x01
#define ACTION_GET_DEV_INFO         0x02
#define ACTION_GET_DEV_DESC         0x04
#define ACTION_GET_CONFIG_DESC      0x08
#define ACTION_GET_STR_DESC         0x10
#define ACTION_CLOSE_DEV            0x20
#define ACTION_EXIT                 0x40
#define ACTION_CLAIM_INTF           0x0100
#define ACTION_TRANSFER_CONTROL     0x0200
#define ACTION_TRANSFER             0x0400

typedef struct {
    usb_host_client_handle_t client_hdl;
    uint8_t dev_addr;
    usb_device_handle_t dev_hdl;
    uint32_t actions;
    uint16_t bMaxPacketSize0;
    usb_ep_desc_t *ep_in;
    usb_ep_desc_t *ep_out;
    SemaphoreHandle_t transfer_done;
    usb_transfer_status_t transfer_status;
} class_driver_t;

static const char *TAG_CLASS = "CLASS";

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    class_driver_t *driver_obj = (class_driver_t *)arg;
    switch (event_msg->event) {
        case USB_HOST_CLIENT_EVENT_NEW_DEV:
            if (driver_obj->dev_addr == 0) {
                driver_obj->dev_addr = event_msg->new_dev.address;
                //Open the device next
                driver_obj->actions |= ACTION_OPEN_DEV;
            }
            break;
        case USB_HOST_CLIENT_EVENT_DEV_GONE:
            if (driver_obj->dev_hdl != NULL) {
                //Cancel any other actions and close the device next
                driver_obj->actions = ACTION_CLOSE_DEV;
            }
            break;
        default:
            //Should never occur
            abort();
    }
}

static void action_open_dev(class_driver_t *driver_obj)
{
    assert(driver_obj->dev_addr != 0);
    ESP_LOGI(TAG_CLASS, "Opening device at address %d", driver_obj->dev_addr);
    ESP_ERROR_CHECK(usb_host_device_open(driver_obj->client_hdl, driver_obj->dev_addr, &driver_obj->dev_hdl));
    
    //Get the device's information next
    driver_obj->actions &= ~ACTION_OPEN_DEV;
    driver_obj->actions |= ACTION_GET_DEV_INFO;
}

static void action_get_info(class_driver_t *driver_obj)
{
    assert(driver_obj->dev_hdl != NULL);
    ESP_LOGI(TAG_CLASS, "Getting device information");
    usb_device_info_t dev_info;
    ESP_ERROR_CHECK(usb_host_device_info(driver_obj->dev_hdl, &dev_info));
    ESP_LOGI(TAG_CLASS, "\t%s speed", (dev_info.speed == USB_SPEED_LOW) ? "Low" : "Full");
    ESP_LOGI(TAG_CLASS, "\tbConfigurationValue %d", dev_info.bConfigurationValue);

    //Get the device descriptor next
    driver_obj->actions &= ~ACTION_GET_DEV_INFO;
    driver_obj->actions |= ACTION_GET_DEV_DESC;
}

static void action_get_dev_desc(class_driver_t *driver_obj)
{
    assert(driver_obj->dev_hdl != NULL);
    ESP_LOGI(TAG_CLASS, "Getting device descriptor");
    const usb_device_desc_t *dev_desc;
    ESP_ERROR_CHECK(usb_host_get_device_descriptor(driver_obj->dev_hdl, &dev_desc));
    ESP_LOGI(TAG_CLASS, "\tidVendor 0x%04x", dev_desc->idVendor);
    ESP_LOGI(TAG_CLASS, "\tidProduct 0x%04x", dev_desc->idProduct);
    usb_print_device_descriptor(dev_desc);

    driver_obj->bMaxPacketSize0 = dev_desc->bMaxPacketSize0;

    //Get the device's config descriptor next
    driver_obj->actions &= ~ACTION_GET_DEV_DESC;
    driver_obj->actions |= ACTION_GET_CONFIG_DESC;
}

static void action_get_config_desc(class_driver_t *driver_obj)
{
    assert(driver_obj->dev_hdl != NULL);
    ESP_LOGI(TAG_CLASS, "Getting config descriptor");
    const usb_config_desc_t *config_desc;
    ESP_ERROR_CHECK(usb_host_get_active_config_descriptor(driver_obj->dev_hdl, &config_desc));
    usb_print_config_descriptor(config_desc, NULL);

    //Get the device's string descriptors next
    driver_obj->actions &= ~ACTION_GET_CONFIG_DESC;
    driver_obj->actions |= ACTION_GET_STR_DESC;
}

static void action_get_str_desc(class_driver_t *driver_obj)
{
    assert(driver_obj->dev_hdl != NULL);
    usb_device_info_t dev_info;
    ESP_ERROR_CHECK(usb_host_device_info(driver_obj->dev_hdl, &dev_info));
    if (dev_info.str_desc_manufacturer) {
        ESP_LOGI(TAG_CLASS, "Getting Manufacturer string descriptor");
        usb_print_string_descriptor(dev_info.str_desc_manufacturer);
    }
    if (dev_info.str_desc_product) {
        ESP_LOGI(TAG_CLASS, "Getting Product string descriptor");
        usb_print_string_descriptor(dev_info.str_desc_product);
    }
    if (dev_info.str_desc_serial_num) {
        ESP_LOGI(TAG_CLASS, "Getting Serial Number string descriptor");
        usb_print_string_descriptor(dev_info.str_desc_serial_num);
    }

    //Claim the interface next
    driver_obj->actions &= ~ACTION_GET_STR_DESC;
    driver_obj->actions |= ACTION_CLAIM_INTF;
}

static void action_claim_interface(class_driver_t *driver_obj)
{
    assert(driver_obj->dev_hdl != NULL);
    ESP_LOGI(TAG_CLASS, "Getting config descriptor");
    const usb_config_desc_t *config_desc;
    ESP_ERROR_CHECK(usb_host_get_active_config_descriptor(driver_obj->dev_hdl, &config_desc));

    bool hidIntfClaimed = false;
    int offset = 0;
    for (size_t n = 0; n < config_desc->bNumInterfaces; n++)
    {
        const usb_intf_desc_t *intf = usb_parse_interface_descriptor(config_desc, n, 0, &offset);
        printf("Parsed intf->bInterfaceNumber: 0x%02x \n", intf->bInterfaceNumber);
        
        if (intf->bInterfaceClass == 0x03) // HID - https://www.usb.org/defined-class-codes
        {
            printf("Detected HID intf->bInterfaceClass: 0x%02x \n", intf->bInterfaceClass);

            const usb_ep_desc_t *ep_in = nullptr;
            const usb_ep_desc_t *ep_out = nullptr;
            const usb_ep_desc_t *ep = nullptr;
            for (size_t i = 0; i < intf->bNumEndpoints; i++) {
                int _offset = 0;
                ep = usb_parse_endpoint_descriptor_by_index(intf, i, config_desc->wTotalLength, &_offset);
                printf("\t > Detected EP num: %d/%d, len: %d, ", i + 1, intf->bNumEndpoints, config_desc->wTotalLength);
                if (ep) {
                    printf("address: 0x%02x, mps: %d, dir: %s", ep->bEndpointAddress, ep->wMaxPacketSize, (ep->bEndpointAddress & 0x80) ? "IN" : "OUT");
                    if (ep->bmAttributes != USB_TRANSFER_TYPE_INTR) {
                        // only support INTERRUPT > IN Report in action_transfer() for now
                        continue;
                    }
                    if (ep->bEndpointAddress & 0x80) {
                        ep_in = ep;
                        driver_obj->ep_in = (usb_ep_desc_t *)ep_in;
                        // printf(", ep_in->addr: 0x%02x", driver_obj->ep_in->bEndpointAddress);
                    } else {
                        ep_out = ep;
                        driver_obj->ep_out = (usb_ep_desc_t *)ep_out;
                        // printf(", ep_out->addr: 0x%02x", driver_obj->ep_out->bEndpointAddress);
                    }
                    printf("\n");
                } else {
                    ESP_LOGW("", "error to parse endpoint by index; EP num: %d/%d, len: %d", i + 1, intf->bNumEndpoints, config_desc->wTotalLength);
                }
            }
            esp_err_t err = usb_host_interface_claim(driver_obj->client_hdl, driver_obj->dev_hdl, n, 0);
            if (err) {
                ESP_LOGI("", "interface claim status: %d", err);
            } else {
                printf("Claimed HID intf->bInterfaceNumber: 0x%02x \n", intf->bInterfaceNumber);
                printf("\n");
                hidIntfClaimed = true;
            }
        }
    }

    //Get the HID's descriptors next
    driver_obj->actions &= ~ACTION_CLAIM_INTF;
    if (hidIntfClaimed)
    {
        driver_obj->actions |= ACTION_TRANSFER_CONTROL;
        // driver_obj->actions |= ACTION_TRANSFER;
    }
}

static void transfer_cb(usb_transfer_t *transfer)
{
    //This is function is called from within usb_host_client_handle_events(). Don't block and try to keep it short
    //struct class_driver_control *class_driver_obj = (struct class_driver_control *)transfer->context;
    // static int PrintDivider = 0;
    // if ((PrintDivider++ % 10) == 0) {
    //     // printf("status %d, actual number of bytes transferred %d\n", transfer->status, transfer->actual_num_bytes);
    //     // for(int i=0; i < transfer->actual_num_bytes; i++)
    //     //     printf("%02X ", transfer->data_buffer[i]);
    //     // printf("\n");
    //     if (PrintDivider >= 10000) PrintDivider = 0;
    // }
    class_driver_t *driver_obj = (class_driver_t *)transfer->context;
    driver_obj->transfer_status = transfer->status;
    xSemaphoreGive(driver_obj->transfer_done);
}

static esp_err_t wait_for_transfer_done(usb_transfer_t *transfer)
{
    class_driver_t *driver_obj = (class_driver_t *)transfer->context;
    BaseType_t received = xSemaphoreTake(driver_obj->transfer_done, pdMS_TO_TICKS(transfer->timeout_ms));
    // BaseType_t received = xSemaphoreTake(driver_obj->transfer_done, portMAX_DELAY);
    if (received != pdTRUE) {
        xSemaphoreGive(driver_obj->transfer_done);
        return ESP_ERR_TIMEOUT;
    }
    xSemaphoreGive(driver_obj->transfer_done);
    return (driver_obj->transfer_status == USB_TRANSFER_STATUS_COMPLETED) ? ESP_OK : ESP_FAIL;
}

static void action_transfer_control(class_driver_t *driver_obj)
{
    #define GET_HID_REPORT_DESC
    // #define GET_REPORT

    assert(driver_obj->dev_hdl != NULL);
    static uint16_t mps = driver_obj->bMaxPacketSize0;
    static uint16_t tps = usb_round_up_to_mps(1024, mps);
    static usb_transfer_t *transfer;
    if (!transfer) {
        usb_host_transfer_alloc(tps, 0, &transfer);
    }
    static int32_t lastSendTime = 0;
    if (xTaskGetTickCount() - lastSendTime > 8)
    {
        usb_setup_packet_t stp;
        #if defined(GET_HID_REPORT_DESC)
            // 0x81,        // bmRequestType: Dir: D2H, Type: Standard, Recipient: Interface
            // 0x06,        // bRequest (Get Descriptor)
            // 0x00,        // wValue[0:7]  Desc Index: 0
            // 0x22,        // wValue[8:15] Desc Type: (HID Report)
            // 0x00, 0x00,  // wIndex Language ID: 0x00
            // 0x40, 0x00,  // wLength = 64
            stp.bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
            stp.bRequest = USB_B_REQUEST_GET_DESCRIPTOR;
            stp.wValue = 0x2200;
            stp.wIndex = 0;
            stp.wLength = tps - 8;
            transfer->num_bytes = tps;

        #elif defined(GET_REPORT)
            // 0xA1,        //   bmRequestType: Dir: D2H, Type: Class, Recipient: Interface
            // 0x01,        //   bRequest
            // 0x00, 0x03,  //   wValue[0:15] = 0x0300
            // 0x00, 0x00,  //   wIndex = 0x00
            // 0x38, 0x00,  //   wLength = 56
            // >>>> A1 01 00 03 00 00 38 00
            stp.bmRequestType = 0xA1;
            stp.bRequest = 0x01;
            stp.wValue = 0x0100;
            stp.wIndex = 0x0000;
            stp.wLength = tps - 8;
            transfer->num_bytes = tps;

        #endif

        memcpy(transfer->data_buffer, &stp, USB_SETUP_PACKET_SIZE);
        for(int i=0; i < 8; i++)
            printf("%02X ", transfer->data_buffer[i]);
        printf("\n");
        printf("transfer->data_buffer_size: %d\n", transfer->data_buffer_size);
        printf("transfer->num_bytes: %d\n", transfer->num_bytes);

        transfer->bEndpointAddress = 0x00;
        printf("transfer->bEndpointAddress: 0x%02X \n", transfer->bEndpointAddress);

        transfer->device_handle = driver_obj->dev_hdl;
        transfer->callback = transfer_cb;
        transfer->context = (void *)driver_obj;
        transfer->timeout_ms = 1000;

        BaseType_t received = xSemaphoreTake(driver_obj->transfer_done, 100);
        if (received == pdTRUE) {
            esp_err_t result = usb_host_transfer_submit_control(driver_obj->client_hdl, transfer);
            if (result != ESP_OK)
                ESP_LOGW("", "attempting control %s", esp_err_to_name(result));
            usb_host_client_handle_events(driver_obj->client_hdl, 10); // for raising transfer->callback
            wait_for_transfer_done(transfer);
            if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
                printf("Transfer failed - Status %d \n", transfer->status);
            }
            // else { printf("Transfer completed - Status %d \n", transfer->status); }
        }

        if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
            printf("usb_host_transfer_submit_control - completed \n");

            #if defined(GET_HID_REPORT_DESC)
                //>>>>> for HID Report Descriptor
                // Explanation: https://electronics.stackexchange.com/questions/68141/
                // USB Descriptor and Request Parser: https://eleccelerator.com/usbdescreqparser/#
                //<<<<<
                printf("\nstatus %d, actual number of bytes transferred %d\n", transfer->status, transfer->actual_num_bytes);
                for(int i=0; i < transfer->actual_num_bytes; i++) {
                    if (i == 8) {
                        printf("\n\n>>> Goto https://eleccelerator.com/usbdescreqparser/ \n");
                        printf(">>> Copy & paste below HEX and parser as... USB HID Report Descriptor\n\n");
                    }
                    printf("%02X ", transfer->data_buffer[i]);
                }
                printf("\n\n");
                uint8_t *const data = (uint8_t *const)(transfer->data_buffer + USB_SETUP_PACKET_SIZE);
                size_t len = transfer->actual_num_bytes - 8;
                printf("HID Report Descriptor\n");
                printf("> size: %ld bytes\n", len);
                uint8_t vdrDefUsagePage[] = { 0x06, 0x00, 0xFF, 0x09, 0x01 };
                uint8_t gamepadUsagePage[] = { 0x05, 0x01, 0x09, 0x05 };
                int retVd = memcmp(data, vdrDefUsagePage, sizeof(vdrDefUsagePage));
                int retGp = memcmp(data, gamepadUsagePage, sizeof(gamepadUsagePage));
                bool isGamepad = retGp == 0;
                bool isVenDef  = retVd == 0;
                printf("> Parsed Usage Page: %s\n", isGamepad ? "HID Gamepad" : isVenDef ? "Vendor Defined" : "Unkown");
                printf("\n\n");

            #elif defined(GET_REPORT)
                printf("\nstatus %d, actual number of bytes transferred %d\n", transfer->status, transfer->actual_num_bytes);
                for(int i=0; i < transfer->actual_num_bytes; i++) {
                    if (i == 8) {
                        printf("\n\n>>> Goto https://eleccelerator.com/usbdescreqparser/ \n");
                        printf(">>> Copy & paste below HEX and parser\n\n");
                    }
                    printf("%02X ", transfer->data_buffer[i]);
                }
                printf("\n\n");
                // uint8_t *const data = (uint8_t *const)(transfer->data_buffer + USB_SETUP_PACKET_SIZE);

            #endif
        }

        driver_obj->actions &= ~ACTION_TRANSFER_CONTROL;
        if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
            driver_obj->actions |= ACTION_TRANSFER;
        }

        vTaskDelay(1); //Short delay to let client task spin up
        lastSendTime = xTaskGetTickCount();
    }
}

static void action_transfer(class_driver_t *driver_obj)
{
    assert(driver_obj->dev_hdl != NULL);
    static uint16_t mps = driver_obj->ep_in->wMaxPacketSize;
    // static uint16_t tps = usb_round_up_to_mps(mps, mps);
    static usb_transfer_t *transfer;
    if (!transfer) {
        usb_host_transfer_alloc(mps, 0, &transfer);
    }
    static int32_t lastSendTime = 0;
    if (xTaskGetTickCount() - lastSendTime > 8)
    {
        transfer->num_bytes = mps;
        memset(transfer->data_buffer, 0x00, mps);
        // printf("transfer->data_buffer_size: %d\n", transfer->data_buffer_size);
        // printf("transfer->num_bytes: %d\n", transfer->num_bytes);

        transfer->bEndpointAddress = driver_obj->ep_in->bEndpointAddress;
        // printf("transfer->bEndpointAddress: 0x%02X \n", transfer->bEndpointAddress);

        transfer->device_handle = driver_obj->dev_hdl;
        transfer->callback = transfer_cb;
        transfer->context = (void *)driver_obj;
        transfer->timeout_ms = 1000;

        BaseType_t received = xSemaphoreTake(driver_obj->transfer_done, 100);
        if (received == pdTRUE) {
            esp_err_t result = usb_host_transfer_submit(transfer);
            if (result != ESP_OK)
                ESP_LOGW("", "attempting control %s", esp_err_to_name(result));
            usb_host_client_handle_events(driver_obj->client_hdl, 10); // for raising transfer->callback
            wait_for_transfer_done(transfer);
            if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
                printf("Transfer failed - Status %d \n", transfer->status);
            }
            // else { printf("Transfer completed - Status %d \n", transfer->status); }
        }

        if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
            // printf("usb_host_transfer_submit - completed \n");
            if (transfer->actual_num_bytes > 0) {
                unsigned char *const data = (unsigned char *const)(transfer->data_buffer);

                #define DEBUG_EP_IN_GET_REPORT
                #if defined(DEBUG_EP_IN_GET_REPORT)
                    //
                    // check HID Report Descriptor for usage, search GET_HID_REPORT_DESC in this file
                    // gist: https://gist.github.com/jledet/2857343
                    //       https://www.microchip.com/forums/m913995.aspx
                    //
                    for (int i=0; i<transfer->actual_num_bytes && i<11; i++) {
                        // printf("%d ", data[i]);
                        // printf("%02X ", data[i]);
                        for (int b = 8; b != -1; b--) printf("%d", (data[i] & (1 << b)) >> b );
                        printf(" ");
                    }
                    printf("\n");
                #endif
            }
        }

        // driver_obj->actions &= ~ACTION_TRANSFER; // break during development

        vTaskDelay(1); //Short delay to let client task spin up
        lastSendTime = xTaskGetTickCount();
    }
}

static void aciton_close_dev(class_driver_t *driver_obj)
{
    const usb_config_desc_t *config_desc;
    ESP_ERROR_CHECK(usb_host_get_active_config_descriptor(driver_obj->dev_hdl, &config_desc));
    
    int offset = 0;
    for (size_t n = 0; n < config_desc->bNumInterfaces; n++)
    {
        const usb_intf_desc_t *intf = usb_parse_interface_descriptor(config_desc, n, 0, &offset);
        printf("\nReleasing intf->bInterfaceNumber: 0x%02x \n", intf->bInterfaceNumber);
        if (intf->bInterfaceClass == 0x03) // HID - https://www.usb.org/defined-class-codes
        {
            printf("\nReleasing HID intf->bInterfaceClass: 0x%02x \n", intf->bInterfaceClass);
            const usb_ep_desc_t *ep_in = nullptr;
            const usb_ep_desc_t *ep_out = nullptr;
            const usb_ep_desc_t *ep = nullptr;
            for (size_t i = 0; i < intf->bNumEndpoints; i++) {
                int _offset = 0;
                ep = usb_parse_endpoint_descriptor_by_index(intf, i, config_desc->wTotalLength, &_offset);
                if (ep) {
                    if (ep->bEndpointAddress & 0x80) {
                        ep_in = ep;
                    } else {
                        ep_out = ep;
                    }
                    printf("\t > Halting EP num: %d/%d, len: %d, ", i + 1, intf->bNumEndpoints, config_desc->wTotalLength);
                    printf("address: 0x%02x, EP max size: %d, dir: %s\n", ep->bEndpointAddress, ep->wMaxPacketSize, (ep->bEndpointAddress & 0x80) ? "IN" : "OUT");
                    ESP_ERROR_CHECK(usb_host_endpoint_halt(driver_obj->dev_hdl, ep->bEndpointAddress));
                    ESP_ERROR_CHECK(usb_host_endpoint_flush(driver_obj->dev_hdl, ep->bEndpointAddress));
                }
            }
            ESP_ERROR_CHECK(usb_host_interface_release(driver_obj->client_hdl, driver_obj->dev_hdl, n));
        }
    }

    ESP_ERROR_CHECK(usb_host_device_close(driver_obj->client_hdl, driver_obj->dev_hdl));
    driver_obj->dev_hdl = NULL;
    driver_obj->dev_addr = 0;
    //We need to exit the event handler loop
    driver_obj->actions &= ~ACTION_CLOSE_DEV;
    driver_obj->actions &= ~ACTION_TRANSFER;
    driver_obj->actions |= ACTION_EXIT;
}

void usb_class_driver_task(void *arg)
{
    SemaphoreHandle_t signaling_sem = (SemaphoreHandle_t)arg;
    class_driver_t driver_obj = {0};

    //Wait until daemon task has installed USB Host Library
    xSemaphoreTake(signaling_sem, portMAX_DELAY);

    ESP_LOGI(TAG_CLASS, "Registering Client");
    usb_host_client_config_t client_config = {
        .is_synchronous = false,    //Synchronous clients currently not supported. Set this to false
        .max_num_event_msg = CLIENT_NUM_EVENT_MSG,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = (void *)&driver_obj,
        },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&client_config, &driver_obj.client_hdl));

    driver_obj.transfer_done = xSemaphoreCreateCounting( 1, 1 );

    while (1) {
        if (driver_obj.actions == 0) {
            usb_host_client_handle_events(driver_obj.client_hdl, portMAX_DELAY);
        } else {
            if (driver_obj.actions & ACTION_OPEN_DEV) {
                action_open_dev(&driver_obj);
            }
            if (driver_obj.actions & ACTION_GET_DEV_INFO) {
                action_get_info(&driver_obj);
            }
            if (driver_obj.actions & ACTION_GET_DEV_DESC) {
                action_get_dev_desc(&driver_obj);
            }
            if (driver_obj.actions & ACTION_GET_CONFIG_DESC) {
                action_get_config_desc(&driver_obj);
            }
            if (driver_obj.actions & ACTION_GET_STR_DESC) {
                action_get_str_desc(&driver_obj);
            }
            if (driver_obj.actions & ACTION_CLAIM_INTF) {
                action_claim_interface(&driver_obj);
            }
            if (driver_obj.actions & ACTION_TRANSFER_CONTROL) {
                action_transfer_control(&driver_obj);
            }
            if (driver_obj.actions & ACTION_TRANSFER) {
                action_transfer(&driver_obj);
            }
            if (driver_obj.actions & ACTION_CLOSE_DEV) {
                aciton_close_dev(&driver_obj);
            }
            if (driver_obj.actions & ACTION_EXIT) {
                break;
            }
        }
    }

    vSemaphoreDelete(driver_obj.transfer_done);

    ESP_LOGI(TAG_CLASS, "Deregistering Client");
    ESP_ERROR_CHECK(usb_host_client_deregister(driver_obj.client_hdl));

    //Wait to be deleted
    xSemaphoreGive(signaling_sem);
    vTaskSuspend(NULL);
}
