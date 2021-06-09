// Copyright 2015-2020 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "unity.h"
#include "test_utils.h"
#include "test_hcd_common.h"

// ------------------------------------------------- Mock HID Mice -----------------------------------------------------

/*
Note: The following test requires that USB low speed mouse be connected. The mouse should...

- Be implement the HID with standard report format used by mice
- It's configuration 1 should have the following endpoint

Endpoint Descriptor:
    bLength             7
    bDescriptorType     5
    bEndpointAddress    0x81  EP 1 IN
    bmAttributes        3
        Transfer Type   Interrupt
        Synch Type      None
        Usage Type      Data
    wMaxPacketSize      0x0004  1x 4 bytes
    bInterval           10

If you're using another mice with different endpoints, modify the endpoint descriptor below
*/
static const usb_desc_ep_t in_ep_desc = {
    .bLength = sizeof(usb_desc_ep_t),
    .bDescriptorType = USB_B_DESCRIPTOR_TYPE_ENDPOINT,
    .bEndpointAddress = 0x81,       //EP 1 IN
    .bmAttributes = USB_BM_ATTRIBUTES_XFER_INT,
    .wMaxPacketSize = 4,            //MPS of 4 bytes
    .bInterval = 10,                //Interval of 10ms
};

typedef union {
    struct {
        uint32_t left_button: 1;
        uint32_t right_button: 1;
        uint32_t middle_button: 1;
        uint32_t reserved5: 5;
        uint8_t x_movement;
        uint8_t y_movement;
    } __attribute__((packed));
    uint8_t val[3];
} mock_hid_mouse_report_t;
_Static_assert(sizeof(mock_hid_mouse_report_t) == 3, "Size of HID mouse report incorrect");

static void mock_hid_process_report(mock_hid_mouse_report_t *report, int iter)
{
    static int x_pos = 0;
    static int y_pos = 0;
    //Update X position
    if (report->x_movement & 0x80) {    //Positive movement
        x_pos += report->x_movement & 0x7F;
    } else {    //Negative movement
        x_pos -= report->x_movement & 0x7F;
    }
    //Update Y position
    if (report->y_movement & 0x80) {    //Positive movement
        y_pos += report->y_movement & 0x7F;
    } else {    //Negative movement
        y_pos -= report->y_movement & 0x7F;
    }
    printf("\rX:%d\tY:%d\tIter: %d\n", x_pos, y_pos, iter);
}

// --------------------------------------------------- Test Cases ------------------------------------------------------

/*
Test HCD interrupt pipe URBs
Purpose:
    - Test that an interrupt pipe can be created
    - URBs can be created and enqueued to the interrupt pipe
    - Interrupt pipe returns HCD_PIPE_EVENT_URB_DONE
    - Test that URBs can be aborted when enqueued

Procedure:
    - Setup HCD and wait for connection
    - Allocate default pipe and enumerate the device
    - Setup interrupt pipe and allocate URBs
    - Enqueue URBs, expect HCD_PIPE_EVENT_URB_DONE, and requeue
    - Stop after fixed number of iterations
    - Deallocate URBs
    - Teardown

Note: Some mice will NAK until it is moved, so try moving the mouse around if this test case gets stuck.
*/

#define TEST_HID_DEV_SPEED                  USB_SPEED_LOW
#define NUM_URBS                            3
#define URB_DATA_BUFF_SIZE                  4       //MPS is 4
#define NUM_URB_ITERS                       (NUM_URBS * 100)

TEST_CASE("Test HCD interrupt pipe URBs", "[hcd][ignore]")
{
    hcd_port_handle_t port_hdl = test_hcd_setup();  //Setup the HCD and port
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);  //Trigger a connection
    TEST_ASSERT_EQUAL(TEST_HID_DEV_SPEED, TEST_HID_DEV_SPEED);
    vTaskDelay(pdMS_TO_TICKS(100)); //Short delay send of SOF (for FS) or EOPs (for LS)

    hcd_pipe_handle_t default_pipe = test_hcd_pipe_alloc(port_hdl, NULL, 0, port_speed); //Create a default pipe (using a NULL EP descriptor)
    uint8_t dev_addr = test_hcd_enum_device(default_pipe);

    //Allocate interrupt pipe and URBS
    hcd_pipe_handle_t intr_pipe = test_hcd_pipe_alloc(port_hdl, &in_ep_desc, dev_addr, port_speed);
    urb_t *urb_list[NUM_URBS];
    for (int i = 0; i < NUM_URBS; i++) {
        urb_list[i] = test_hcd_alloc_urb(0, URB_DATA_BUFF_SIZE);
        urb_list[i]->transfer.num_bytes = URB_DATA_BUFF_SIZE;
        urb_list[i]->transfer.context = URB_CONTEXT_VAL;
    }

    //Enqueue URBs
    for (int i = 0; i < NUM_URBS; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(intr_pipe, urb_list[i]));
    }
    int iter_count = NUM_URB_ITERS;
    for (iter_count = NUM_URB_ITERS; iter_count > 0; iter_count--) {
        //Wait for an URB to be done
        test_hcd_expect_pipe_event(intr_pipe, HCD_PIPE_EVENT_URB_DONE);
        //Dequeue the URB and check results
        urb_t *urb = hcd_urb_dequeue(intr_pipe);
        TEST_ASSERT_EQUAL(USB_TRANSFER_STATUS_COMPLETED, urb->transfer.status);
        TEST_ASSERT_EQUAL(URB_CONTEXT_VAL, urb->transfer.context);
        mock_hid_process_report((mock_hid_mouse_report_t *)urb->transfer.data_buffer, iter_count);
        //Requeue URB
        if (iter_count > NUM_URBS) {
            TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(intr_pipe, urb));
        }
    }

    //Free URB list and pipe
    for (int i = 0; i < NUM_URBS; i++) {
        test_hcd_free_urb(urb_list[i]);
    }
    test_hcd_pipe_free(intr_pipe);
    test_hcd_pipe_free(default_pipe);
    //Clearnup
    test_hcd_wait_for_disconn(port_hdl, false);
    test_hcd_teardown(port_hdl);
}
