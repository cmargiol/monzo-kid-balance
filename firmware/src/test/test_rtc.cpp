/**
 * @file test_rtc.cpp
 * @author Forairaaaaa
 * @brief
 * @version 0.1
 * @date 2023-05-26
 *
 * @copyright Copyright (c) 2023
 *
 */
#include "test.h"
// #include "../../lib/rtc/cplus_RTC.h"

namespace TEST
{

    cplus_RTC _rtc;

    /**
     * "Fri 5 Sep 2026" for the clock screen, or a note that the clock has
     * never been set. The chip raises VL (bit 7 of the seconds register)
     * whenever its supply has dropped far enough to lose the count, and
     * clears it on the next write; until then the date registers hold
     * whatever they powered up with, which is not always out of range, so
     * the flag is the only reliable way to avoid printing a date that looks
     * real. The range check catches a corrupted register that kept its flag.
     * Returns a pointer to a static buffer: UI task only.
     */
    static const char *date_line(const RTC_DateTypeDef &d)
    {
        static const char *DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        static const char *MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        static char line[24];
        bool never_set = _rtc.ReadReg(0x02) & 0x80;
        if (never_set || d.Year < 2024 || d.Year > 2100 || d.Month < 1 ||
            d.Month > 12 || d.WeekDay > 6 || d.Date < 1 || d.Date > 31)
            return "clock not set yet";
        snprintf(line, sizeof(line), "%s %d %s %d", DAYS[d.WeekDay], d.Date,
                 MONTHS[d.Month - 1], d.Year);
        return line;
    }

    void TEST::DisplayRTC()
    {

        display->setFont(&fonts::Font0);

        Disbuff->fillRect(0, 0, 240, 135, Disbuff->color565(0, 0, 0));
        // Displaybuff();
        _rtc.GetBm8563Time();
        RTC_TimeTypeDef time;
        RTC_DateTypeDef date;
        _rtc.GetTime(&time);

        Disbuff->setTextSize(4);
        Disbuff->setCursor(6, 25);
        Disbuff->setTextColor(TFT_WHITE);

        while (1)
        {
            Disbuff->fillRect(0, 0, 240, 135, Disbuff->color565(0, 0, 0));
            _rtc.GetTime(&time);
            _rtc.GetDate(&date);
            Disbuff->setTextSize(4);
            Disbuff->setTextColor(TFT_WHITE);
            Disbuff->setCursor(25, 48);
            Disbuff->printf("%02d:%02d:%02d", time.Hours, time.Minutes,
                            time.Seconds);
            if (!is_test_mode) // the factory prompt occupies this band
            {
                Disbuff->setTextSize(2);
                Disbuff->setTextColor(TFT_LIGHTGREY);
                Disbuff->drawCenterString(date_line(date), 120, 96);
            }
            Disbuff->fillRect(0, 0, 240, 25, Disbuff->color565(20, 20, 20));
            Disbuff->setTextSize(2);
            Disbuff->setTextColor(TFT_WHITE);
            Disbuff->drawCenterString("CLOCK", 120, 5);

            if (is_test_mode)
            {
                display->setFont(&fonts::efontCN_24);
                display->setTextColor(TFT_YELLOW, TFT_BLACK);
                display->setTextSize(1);
                display->setCursor(0, 80);
                display->printf("确保数值正常且变化");
                display->setFont(&fonts::Font0);
            }

            Displaybuff();

            // M5.update();
            // checkAXPPress();
            delay(100);

            checkReboot();
            if (checkNext())
            {
                break;
            }
        }
    }

    /**
     * Copy the ESP32's local time (set from NTP, timezone DEVICE_TZ) into
     * the clock chip, which nothing else ever sets — it restarts from
     * 00:00:00 whenever the battery runs flat. A no-op until the system
     * clock has been synced. Called on the UI task only: the chip shares
     * the I2C bus with the motion sensor.
     */
    void rtc_set_from_localtime()
    {
        struct tm t;
        if (!getLocalTime(&t, 0)) // 0: don't wait for a sync, just report
            return;
        RTC_DateTypeDef rd = {(uint8_t)t.tm_wday, (uint8_t)(t.tm_mon + 1), (uint8_t)t.tm_mday,
                              (uint16_t)(t.tm_year + 1900)};
        RTC_TimeTypeDef rt = {(uint8_t)t.tm_hour, (uint8_t)t.tm_min, (uint8_t)t.tm_sec};
        // Date first: the chip keeps counting between the two writes, and
        // this order can't leave the date a day behind across midnight.
        _rtc.SetDate(&rd);
        _rtc.SetTime(&rt);
    }

    void TEST::rtc_init()
    {
        // rtc.begin();
        _rtc.clearIRQ();
        _rtc.disableIRQ();
    }

    void TEST::rtc_test()
    {
        printf("rtc test\n");
        DisplayRTC();
        printf("quit rtc test\n");
    }

    void TEST::rtc_wakeup_test()
    {
        display->fillScreen(TFT_BLACK);
        display->setCursor(0, 10);
        display->setFont(&fonts::efontCN_24);
        display->setTextColor(TFT_YELLOW);
        display->setTextSize(1);
        display->printf(" [RTC 唤醒测试]\n - 请移除电源 -\n\n 按下按键[关机]\n 数秒后将自动启动");
        displayUpdate();

        while (1)
        {
            if (btnA.pressed())
            {
                _tone(5000, 50);
                break;
            }
            if (btnB.pressed())
            {
                _tone(5500, 50);
                break;
            }
            if (btnPWR.pressed())
            {
                _tone(3500, 50);
                break;
            }
        }

        // waitNext();
        _rtc.clearIRQ();
        _rtc.SetAlarmIRQ(4);
        display->fillScreen(TFT_BLACK);
        displayUpdate();
        delay(500);
        // power_off();

        digitalWrite(POWER_HOLD_PIN, 0);

        while (1)
        {
            printf("%d\n", btnPWR.read());
            delay(50);
        }
    }

    // void TEST::gpio_test()
    // {
    //     printf("hat test\n");

    //     display->fillScreen(TFT_BLACK);
    //     display->setCursor(0, 5);
    //     display->setFont(&fonts::efontCN_24);
    //     display->setTextColor(TFT_YELLOW);
    //     display->setTextSize(1);
    //     display->printf("[HAT接口测试]\n\n请插上治具\n观察 [流水灯]\n和 [主机LED灯]");
    //     displayUpdate();

    //     // gpio_reset_pin(GPIO_NUM_19);
    //     // pinMode(19, OUTPUT);
    //     // while (1) {
    //     //     digitalWrite(19, 0);
    //     //     printf("19 %d\n", digitalRead(19));
    //     //     delay(1000);
    //     //     digitalWrite(19, 1);
    //     //     printf("19 %d\n", digitalRead(19));
    //     //     delay(1000);
    //     // }

    //     std::vector<gpio_num_t> gpio_list = {GPIO_NUM_26, GPIO_NUM_25, GPIO_NUM_0, GPIO_NUM_19};

    //     /* Reset */
    //     for (auto i : gpio_list) {
    //         gpio_reset_pin(i);
    //         pinMode(i, OUTPUT);
    //     }

    //     uint32_t time_count = 0;
    //     auto iter = gpio_list.begin();
    //     while (1) {

    //         if ((millis() - time_count) > 600) {

    //             printf("set %d %d\n", *iter, !digitalRead(*iter));
    //             digitalWrite(*iter, !digitalRead(*iter));

    //             iter++;
    //             if (iter == gpio_list.end()) {
    //                 iter = gpio_list.begin();
    //             }

    //             time_count = millis();
    //         }

    //         checkReboot();
    //         if (checkNext()) {
    //             break;
    //         }

    //     }

    //     /* Reset */
    //     for (auto i : gpio_list) {
    //         gpio_reset_pin(i);
    //     }

    //     display->setFont(&fonts::Font0);
    //     display->setTextSize(1);

    //     printf("quit hat test\n");

    // }

}
