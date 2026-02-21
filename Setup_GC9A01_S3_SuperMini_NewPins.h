// Setup_GC9A01_S3_SuperMini_NewPins.h

#define USER_SETUP_ID 1000
#define USER_SETUP_INFO "ESP32-S3 SuperMini GC9A01 round 240x240 - pins 3,4,5,6,7"

// Fix StoreProhibited crash (0x10) bij tft.init() op ESP32-S3 met nieuwere Arduino core
#define USE_HSPI_PORT

#define GC9A01_DRIVER

#define TFT_WIDTH  240
#define TFT_HEIGHT 240

#define TFT_MISO -1        // niet gebruikt
#define TFT_MOSI 6         // SDA
#define TFT_SCLK 7         // SCL
#define TFT_CS   4         // CS
#define TFT_DC   5         // DO = DC
#define TFT_RST  3         // RST

// Backlight als je een aparte BL-pin hebt (optioneel, anders direct naar 3.3V)
// #define TFT_BL   21      // voorbeeld GPIO, pas aan als je dimmen wilt
// #define TFT_BACKLIGHT_ON HIGH

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define SMOOTH_FONT

#define SPI_FREQUENCY  27000000   // 27 MHz stabiel, probeer later 40 MHz als het werkt
//#define SPI_FREQUENCY  40000000

#define TFT_INVERSION_ON    // kleuren geïnverteerd