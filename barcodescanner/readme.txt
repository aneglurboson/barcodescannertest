
#include <cstdio>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "lwip/tcp.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"

// Wi-Fi credentials
#define WIFI_SSID       "name"
#define WIFI_PASSWORD   "password"

// Windows laptop TCP server
#define SERVER_IP       "ip"
#define SERVER_PORT     5000

#define WIFI_TIMEOUT_MS 30000

// TCP status flags
volatile bool tcp_connected = false;
volatile bool message_sent = false;
volatile bool tcp_failed = false;
volatile bool server_disconnected = false;

volatile err_t tcp_error_code = ERR_OK;

struct tcp_pcb* client_pcb = nullptr;

// LED functions
void ledOn()
{
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
}

void ledOff()
{
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
}

void blinkLED(int count, int duration)
{
    for (int i = 0; i < count; i++)
    {
        ledOn();
        sleep_ms(duration);
        ledOff();
        sleep_ms(duration);
    }
}

// TCP connection successful callback
err_t tcpConnectedCallback(
    void* arg,
    struct tcp_pcb* pcb,
    err_t error
)
{
    if (error != ERR_OK)
    {
        tcp_error_code = error;
        tcp_failed = true;
        return ERR_OK;
    }

    tcp_connected = true;

    const char message[] = "PICO TEST MESSAGE\r\n";

    err_t result = tcp_write(
        pcb,
        message,
        sizeof(message) - 1,
        TCP_WRITE_FLAG_COPY
    );

    if (result == ERR_OK)
    {
        result = tcp_output(pcb);
    }

    if (result == ERR_OK)
    {
        message_sent = true;
    }
    else
    {
        tcp_error_code = result;
        tcp_failed = true;
    }

    return ERR_OK;
}

// TCP error callback
void tcpErrorCallback(void* arg, err_t error)
{
    client_pcb = nullptr;
    tcp_error_code = error;
    tcp_failed = true;
}

// Handle server disconnection
err_t tcpReceiveCallback(
    void* arg,
    struct tcp_pcb* pcb,
    struct pbuf* packet,
    err_t error
)
{
    if (packet == nullptr)
    {
        server_disconnected = true;
        return ERR_OK;
    }

    tcp_recved(pcb, packet->tot_len);
    pbuf_free(packet);

    return ERR_OK;
}

// Start TCP connection
bool startTCPClient()
{
    ip_addr_t serverAddress;

    if (!ip4addr_aton(SERVER_IP, &serverAddress))
    {
        printf("[ERROR] Invalid server IP address!\n");
        return false;
    }

    printf("\n[TCP] Creating TCP connection...\n");
    printf("[TCP] Server IP: %s\n", SERVER_IP);
    printf("[TCP] Server port: %d\n", SERVER_PORT);

    cyw43_arch_lwip_begin();

    client_pcb = tcp_new();

    if (client_pcb == nullptr)
    {
        cyw43_arch_lwip_end();
        printf("[ERROR] Could not create TCP client!\n");
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

    if (result != ERR_OK)
    {
        printf("[ERROR] TCP connection start failed: %d\n", result);
        return false;
    }

    printf("[TCP] Connection request submitted!\n");
    return true;
}

int main()
{
    stdio_init_all();
    sleep_ms(3000);

    printf("\n");
    printf("====================================\n");
    printf("       PICO 2 W TCP CLIENT\n");
    printf("====================================\n");

    // Initialize Wi-Fi
    printf("[INIT] Initializing Wi-Fi...\n");

    if (cyw43_arch_init())
    {
        printf("[ERROR] Wi-Fi initialization failed!\n");

        while (true)
        {
            sleep_ms(1000);
        }
    }

    printf("[OK] Wi-Fi hardware initialized!\n");

    blinkLED(3, 150);

    cyw43_arch_enable_sta_mode();

    // Connect to Wi-Fi
    printf("[WIFI] Connecting to %s...\n", WIFI_SSID);

    ledOn();

    int wifiResult = cyw43_arch_wifi_connect_timeout_ms(
        WIFI_SSID,
        WIFI_PASSWORD,
        CYW43_AUTH_WPA2_MIXED_PSK,
        WIFI_TIMEOUT_MS
    );

    if (wifiResult != 0)
    {
        printf("[ERROR] Wi-Fi connection failed: %d\n", wifiResult);

        while (true)
        {
            blinkLED(5, 150);
            sleep_ms(1000);
        }
    }

    printf("[OK] Wi-Fi connected!\n");
    printf("[WIFI] Ready to connect to Windows laptop.\n");

    // Start TCP connection
    if (!startTCPClient())
    {
        printf("[ERROR] Could not start TCP client.\n");

        while (true)
        {
            blinkLED(4, 150);
            sleep_ms(1000);
        }
    }

    // Monitor TCP status
    while (true)
    {
        if (tcp_connected && message_sent)
        {
            printf("\n[SUCCESS] TCP CONNECTED!\n");
            printf("[SUCCESS] Test message sent!\n");
            printf("[MESSAGE] PICO TEST MESSAGE\n");

            ledOn();

            // Print once only
            message_sent = false;
        }

        if (tcp_failed)
        {
            printf("[TCP ERROR] Connection or transmission failed: %d\n",
                   (int)tcp_error_code);

            ledOff();
            tcp_failed = false;
        }

        if (server_disconnected)
        {
            printf("[TCP] Server disconnected.\n");
            ledOff();
            server_disconnected = false;
        }

        sleep_ms(100);
    }

    return 0;
}
