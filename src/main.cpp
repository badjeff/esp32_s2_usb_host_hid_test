#include <Arduino.h>

#include "usb_host_lib_daemon.hpp"
#include "usb_class_driver.hpp"

#define DAEMON_TASK_PRIORITY    2
#define DAEMON_TASK_COREID      0

#define CLASS_TASK_PRIORITY     3
#define CLASS_TASK_COREID       0

void setup(void)
{
    delay(2000); // await monitor port wakeup

    SemaphoreHandle_t signaling_sem = xSemaphoreCreateBinary();

    //Create usb host lib daemon task
    TaskHandle_t daemon_task_hdl;
    xTaskCreatePinnedToCore(usb_host_lib_daemon_task,
                            "usb_host_daemon",
                            4096,
                            (void *)signaling_sem,
                            DAEMON_TASK_PRIORITY,
                            &daemon_task_hdl,
                            DAEMON_TASK_COREID);

    //Create usb class driver task
    TaskHandle_t class_driver_task_hdl;
    xTaskCreatePinnedToCore(usb_class_driver_task,
                            "usb_class_driver",
                            4096,
                            (void *)signaling_sem,
                            CLASS_TASK_PRIORITY,
                            &class_driver_task_hdl,
                            CLASS_TASK_COREID);

    vTaskDelay(10);     //Add a short delay to let the tasks run

    //Wait for the tasks to complete
    for (int i = 0; i < 2; i++) {
        xSemaphoreTake(signaling_sem, portMAX_DELAY);
    }

    //Delete the tasks
    vTaskDelete(class_driver_task_hdl);
    vTaskDelete(daemon_task_hdl);
}

void loop() {
}
