
#include <cstdio>
#include <cstring>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "tusb.h"

#include "lwip/tcp.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"

// ==========================================
// Wi-Fi settings
// ==========================================

#define WIFI_SSID        "atharva"
#define WIFI_PASSWORD    "hotspot@2003"
#define WIFI_TIMEOUT_MS  30000

// ==========================================
// Windows Python TCP server
// ==========================================

#define SERVER_IP        "10.202.255.169"
#define SERVER_PORT      5000

// ==========================================
// Barcode settings
// ==========================================

#define BARCODE_SIZE 128
#define MESSAGE_SIZE 512

char barcode[BARCODE_SIZE] = {};
uint16_t barcode_length = 0;
bool barcode_overflow = false;

char pending_message[MESSAGE_SIZE] = {};
volatile bool message_ready = false;

// ==========================================
// Status flags
// ==========================================

volatile bool wifi_connected = false;
volatile bool tcp_connected = false;
volatile bool tcp_failed = false;
volatile bool server_disconnected = false;

volatile bool scanner_detected = false;
volatile bool fatal_error = false;

volatile err_t tcp_error_code = ERR_OK;

struct tcp_pcb* client_pcb = nullptr;

volatile bool tcp_retry_needed = true;

uint32_t next_tcp_attempt = 0;

#define TCP_RETRY_INTERVAL_MS 3000
// ==========================================
// LED status
// ==========================================

void ledOn()
{
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
}

void ledOff()
{
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
}

void updateStatusLED()
{
    static uint8_t activePattern = 0;
    static uint8_t step = 0;
    static uint32_t nextUpdate = 0;

    uint8_t pattern;

    if (fatal_error)
        pattern = 5;
    else if (scanner_detected)
        pattern = 4;
    else if (tcp_connected)
        pattern = 3;
    else if (wifi_connected)
        pattern = 2;
    else
        pattern = 1;

    uint32_t now =
        to_ms_since_boot(get_absolute_time());

    if (pattern != activePattern)
    {
        activePattern = pattern;
        step = 0;
        nextUpdate = now;
        ledOff();
    }

    if (now < nextUpdate)
        return;

    if (step < pattern * 2)
    {
        if (step % 2 == 0)
            ledOn();
        else
            ledOff();

        step++;
        nextUpdate = now + 120;
    }
    else
    {
        ledOff();
        step = 0;
        nextUpdate = now + 900;
    }
}

// ==========================================
// HID keycode to ASCII
// ==========================================

char keycodeToAscii(uint8_t keycode, bool shift)
{
    if (keycode >= 0x04 && keycode <= 0x1D)
    {
        char c = 'a' + (keycode - 0x04);

        if (shift)
            c = c - 'a' + 'A';

        return c;
    }

    if (keycode >= 0x1E && keycode <= 0x27)
    {
        const char normal[] = "1234567890";
        const char shifted[] = "!@#$%^&*()";

        return shift
            ? shifted[keycode - 0x1E]
            : normal[keycode - 0x1E];
    }

    switch (keycode)
    {
        case 0x2C: return ' ';

        case 0x2D: return shift ? '_' : '-';
        case 0x2E: return shift ? '+' : '=';
        case 0x2F: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31: return shift ? '|' : '\\';

        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x35: return shift ? '~' : '`';

        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';

        default: return 0;
    }
}

// ==========================================
// Barcode input
// ==========================================

void processBarcodeCharacter(char c)
{
    if (c == 0)
        return;

    if (barcode_length < BARCODE_SIZE - 1)
    {
        barcode[barcode_length++] = c;
    }
    else
    {
        barcode_overflow = true;
    }
}

// ==========================================
// Validation helpers
// ==========================================

bool isAlphaNumericField(const char* value)
{
    if (value == nullptr || value[0] == '\0')
        return false;

    for (size_t i = 0; value[i] != '\0'; i++)
    {
        char c = value[i];

        bool valid =
            (c >= '0' && c <= '9') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z');

        if (!valid)
            return false;
    }

    return true;
}

bool isNumericField(const char* value)
{
    if (value == nullptr || value[0] == '\0')
        return false;

    for (size_t i = 0; value[i] != '\0'; i++)
    {
        if (value[i] < '0' || value[i] > '9')
            return false;
    }

    return true;
}

// ==========================================
// Parse compact scanner output
//
// Example:
// 9100110260915AX4210001
//
// 91 + 001 + 10 + 260915AX4 + 21 + 0001
//
// Product code: exactly 3 digits
// Packet number: exactly 4 digits
// Batch number: 1 to 20 alphanumeric characters
// ==========================================

bool parseCompactGS1(
    const char* input,
    char* product,
    char* batch,
    char* packet
)
{
    size_t length = strlen(input);

    // Minimum:
    // 91 + 3 product digits + 10 + 1 batch char + 21 + 4 packet digits
    if (length < 13)
        return false;

    // Must start with AI 91
    if (input[0] != '9' || input[1] != '1')
        return false;

    // Product code occupies exactly 3 digits
    if (input[5] != '1' || input[6] != '0')
        return false;

    memcpy(product, input + 2, 3);
    product[3] = '\0';

    if (!isNumericField(product))
        return false;

    // AI 21 is immediately before the 4-digit packet number
    // Therefore its position is length - 6.
    if (length < 13)
        return false;

    size_t packetMarker = length - 6;

    if (input[packetMarker] != '2' ||
        input[packetMarker + 1] != '1')
        return false;

    // Batch starts immediately after AI 10
    const char* batchStart = input + 7;

    size_t batchLength = packetMarker - 7;

    if (batchLength < 1 || batchLength > 20)
        return false;

    memcpy(batch, batchStart, batchLength);
    batch[batchLength] = '\0';

    // Packet number is the final 4 characters
    memcpy(packet, input + packetMarker + 2, 4);
    packet[4] = '\0';

    return
        isAlphaNumericField(batch) &&
        isNumericField(packet);
}

// ==========================================
// Parse parenthesized representation
//
// Example:
// (91)001(10)260915AX4(21)0001
// ==========================================

bool parseParenthesizedGS1(
    const char* input,
    char* product,
    char* batch,
    char* packet
)
{
    if (strncmp(input, "(91)", 4) != 0)
        return false;

    const char* productStart = input + 4;

    const char* batchMarker = strstr(productStart, "(10)");

    if (batchMarker == nullptr)
        return false;

    const char* batchStart = batchMarker + 4;

    const char* packetMarker = strstr(batchStart, "(21)");

    if (packetMarker == nullptr)
        return false;

    const char* packetStart = packetMarker + 4;

    size_t productLength = batchMarker - productStart;
    size_t batchLength = packetMarker - batchStart;
    size_t packetLength = strlen(packetStart);

    if (productLength != 3)
        return false;

    if (batchLength < 1 || batchLength > 20)
        return false;

    if (packetLength != 4)
        return false;

    memcpy(product, productStart, 3);
    product[3] = '\0';

    memcpy(batch, batchStart, batchLength);
    batch[batchLength] = '\0';

    memcpy(packet, packetStart, 4);
    packet[4] = '\0';

    return
        isNumericField(product) &&
        isAlphaNumericField(batch) &&
        isNumericField(packet);
}

// ==========================================
// Parse GS-separated representation
//
// 91 + product + GS + 10 + batch + GS + 21 + packet
// GS = ASCII 29
// ==========================================

bool parseSeparatedGS1(
    const char* input,
    char* product,
    char* batch,
    char* packet
)
{
    if (input[0] != '9' || input[1] != '1')
        return false;

    const char* productStart = input + 2;

    const char* firstSeparator =
        strchr(productStart, 0x1D);

    if (firstSeparator == nullptr)
        return false;

    if (firstSeparator[1] != '1' ||
        firstSeparator[2] != '0')
        return false;

    const char* batchStart = firstSeparator + 3;

    const char* secondSeparator =
        strchr(batchStart, 0x1D);

    if (secondSeparator == nullptr)
        return false;

    if (secondSeparator[1] != '2' ||
        secondSeparator[2] != '1')
        return false;

    const char* packetStart = secondSeparator + 3;

    size_t productLength = firstSeparator - productStart;
    size_t batchLength = secondSeparator - batchStart;
    size_t packetLength = strlen(packetStart);

    if (productLength != 3)
        return false;

    if (batchLength < 1 || batchLength > 20)
        return false;

    if (packetLength != 4)
        return false;

    memcpy(product, productStart, 3);
    product[3] = '\0';

    memcpy(batch, batchStart, batchLength);
    batch[batchLength] = '\0';

    memcpy(packet, packetStart, 4);
    packet[4] = '\0';

    return
        isNumericField(product) &&
        isAlphaNumericField(batch) &&
        isNumericField(packet);
}

// ==========================================
// Validate barcode and prepare output
// ==========================================

void prepareBarcodeMessage(const char* input)
{
    char product[4] = {};
    char batch[21] = {};
    char packet[5] = {};

    bool valid = false;

    // Try all supported representations
    valid = parseParenthesizedGS1(
        input, product, batch, packet
    );

    if (!valid)
    {
        valid = parseSeparatedGS1(
            input, product, batch, packet
        );
    }

    if (!valid)
    {
        valid = parseCompactGS1(
            input, product, batch, packet
        );
    }

    if (valid)
    {
        snprintf(
            pending_message,
            MESSAGE_SIZE,
            "[ACCEPTABLE GS1 BARCODE]\n"
            "Raw: %s\n"
            "Product code (91): %s\n"
            "Batch number (10): %s\n"
            "Packet number (21): %s\n",
            input,
            product,
            batch,
            packet
        );
    }
    else
    {
        snprintf(
            pending_message,
            MESSAGE_SIZE,
            "[UNACCEPTABLE BARCODE]\n"
            "Raw: %s\n"
            "Expected format: 91 + 3-digit product + "
            "10 + batch + 21 + 4-digit packet.\n",
            input
        );
    }

    message_ready = true;
}

// ==========================================
// Finish barcode
// ==========================================

void finishBarcode()
{
    if (barcode_length == 0)
        return;

    if (barcode_overflow)
    {
        snprintf(
            pending_message,
            MESSAGE_SIZE,
            "[UNACCEPTABLE BARCODE]\n"
            "Reason: Barcode too long.\n"
        );

        message_ready = true;
    }
    else
    {
        barcode[barcode_length] = '\0';
        prepareBarcodeMessage(barcode);
    }

    barcode_length = 0;
    barcode_overflow = false;

    memset(barcode, 0, sizeof(barcode));
}

// ==========================================
// Previous keyboard report
// ==========================================

static hid_keyboard_report_t previous_report = {};

// ==========================================
// TinyUSB callbacks
// ==========================================

extern "C" void tuh_hid_mount_cb(
    uint8_t dev_addr,
    uint8_t instance,
    uint8_t const* desc_report,
    uint16_t desc_len
)
{
    (void)desc_report;
    (void)desc_len;

    scanner_detected = true;

    previous_report = {};

    tuh_hid_receive_report(dev_addr, instance);
}

extern "C" void tuh_hid_umount_cb(
    uint8_t dev_addr,
    uint8_t instance
)
{
    (void)dev_addr;
    (void)instance;

    scanner_detected = false;

    previous_report = {};

    barcode_length = 0;
    barcode_overflow = false;

    memset(barcode, 0, sizeof(barcode));
}

extern "C" void tuh_hid_report_received_cb(
    uint8_t dev_addr,
    uint8_t instance,
    uint8_t const* report,
    uint16_t len
)
{
    if (
        tuh_hid_interface_protocol(dev_addr, instance)
            == HID_ITF_PROTOCOL_KEYBOARD
        &&
        len >= sizeof(hid_keyboard_report_t)
    )
    {
        const hid_keyboard_report_t* keyboard =
            reinterpret_cast<const hid_keyboard_report_t*>(report);

        bool shift =
            (keyboard->modifier & 0x22) != 0;

        for (uint8_t i = 0; i < 6; i++)
        {
            uint8_t key = keyboard->keycode[i];

            if (key == 0)
                continue;

            bool already_pressed = false;

            for (uint8_t j = 0; j < 6; j++)
            {
                if (previous_report.keycode[j] == key)
                {
                    already_pressed = true;
                    break;
                }
            }

            if (already_pressed)
                continue;

            // Enter completes the scan
            if (key == 0x28 || key == 0x58)
            {
                finishBarcode();
            }
            else
            {
                char c = keycodeToAscii(key, shift);
                processBarcodeCharacter(c);
            }
        }

        previous_report = *keyboard;
    }

    tuh_hid_receive_report(dev_addr, instance);
}

// ==========================================
// TCP callbacks
// ==========================================

err_t tcpConnectedCallback(
    void* arg,
    struct tcp_pcb* pcb,
    err_t error
)
{
    (void)arg;
    (void)pcb;

    if (error != ERR_OK)
    {
        tcp_error_code = error;
        tcp_failed = true;
        return ERR_OK;
    }

    tcp_connected = true;

    return ERR_OK;
}

void tcpErrorCallback(void* arg, err_t error)
{
    (void)arg;

    client_pcb = nullptr;
    tcp_connected = false;

    tcp_error_code = error;
    tcp_failed = true;
}

err_t tcpReceiveCallback(
    void* arg,
    struct tcp_pcb* pcb,
    struct pbuf* packet,
    err_t error
)
{
    (void)arg;
    (void)error;

    if (packet == nullptr)
    {
        server_disconnected = true;
        tcp_connected = false;
        client_pcb = nullptr;

        return ERR_OK;
    }

    tcp_recved(pcb, packet->tot_len);
    pbuf_free(packet);

    return ERR_OK;
}

// ==========================================
// Start TCP client
// ==========================================

bool startTCPClient()
{
    ip_addr_t serverAddress;

    if (!ip4addr_aton(SERVER_IP, &serverAddress))
        return false;

    cyw43_arch_lwip_begin();

    client_pcb = tcp_new();

    if (client_pcb == nullptr)
    {
        cyw43_arch_lwip_end();
        return false;
    }

    tcp_arg(client_pcb, nullptr);
    tcp_err(client_pcb, tcpErrorCallback);
    tcp_recv(client_pcb, tcpReceiveCallback);

    err_t result = tcp_connect(
        client_pcb,
        &serverAddress,
        SERVER_PORT,
        tcpConnectedCallback
    );

    cyw43_arch_lwip_end();

    return result == ERR_OK;
}

// ==========================================
// Send result to Python
// ==========================================

void sendPendingMessage()
{
    if (!message_ready ||
        !tcp_connected ||
        client_pcb == nullptr)
        return;

    uint16_t length = strlen(pending_message);

    if (length == 0 || length >= MESSAGE_SIZE)
    {
        message_ready = false;
        return;
    }

    char message[MESSAGE_SIZE + 2];

    memcpy(message, pending_message, length);

    message[length] = '\r';
    message[length + 1] = '\n';

    cyw43_arch_lwip_begin();

    err_t result = tcp_write(
        client_pcb,
        message,
        length + 2,
        TCP_WRITE_FLAG_COPY
    );

    if (result == ERR_OK)
        result = tcp_output(client_pcb);

    cyw43_arch_lwip_end();

    if (result == ERR_OK)
    {
        message_ready = false;
        memset(pending_message, 0, sizeof(pending_message));
    }
    else if (result != ERR_MEM)
    {
        tcp_error_code = result;
        tcp_failed = true;
    }
}

// ==========================================
// MAIN
// ==========================================

int main()
{
    stdio_init_all();

    if (cyw43_arch_init())
    {
        fatal_error = true;

        while (true)
            sleep_ms(100);
    }

    cyw43_arch_enable_sta_mode();

    // Initialize USB Host
    const tusb_rhport_init_t usb_init = {
        .role = TUSB_ROLE_HOST,
        .speed = TUSB_SPEED_FULL
    };

    if (!tuh_rhport_init(0, &usb_init))
    {
        fatal_error = true;

        while (true)
        {
            updateStatusLED();
            sleep_ms(1);
        }
    }

    // Connect Wi-Fi
    int wifiResult = cyw43_arch_wifi_connect_timeout_ms(
        WIFI_SSID,
        WIFI_PASSWORD,
        CYW43_AUTH_WPA2_MIXED_PSK,
        WIFI_TIMEOUT_MS
    );

    if (wifiResult != 0)
    {
        fatal_error = true;
    }
    else
    {
        wifi_connected = true;

        if (!startTCPClient())
            fatal_error = true;
    }

    // Main loop
    while (true)
    {
        tuh_task();

        if (tcp_failed)
        {
            fatal_error = true;
            tcp_failed = false;
        }

        if (server_disconnected)
        {
            fatal_error = true;
            server_disconnected = false;
        }

        sendPendingMessage();

        updateStatusLED();

        sleep_ms(1);
    }

    return 0;
}
