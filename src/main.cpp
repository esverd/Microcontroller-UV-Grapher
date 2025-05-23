#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h> // Hardware-specific library

// Create an instance of the TFT_eSPI class
TFT_eSPI tft = TFT_eSPI();

void setup() {
  Serial.begin(115200); // Initialize serial communication
  Serial.println("Hello! TTGO T-Display Test");

  // Initialize the TFT screen
  tft.init();
  Serial.println("TFT Initialized");

  // Set screen rotation (0, 1, 2, or 3)
  // 0 & 2 are portrait, 1 & 3 are landscape
  // For TTGO T-Display, 1 (landscape) often works well with USB port down.
  // Or 3 (landscape) with USB port up.
  tft.setRotation(1); 
  Serial.println("Rotation Set");

  // Fill the screen with a black color
  tft.fillScreen(TFT_BLACK);
  Serial.println("Screen Filled Black");

  // Set text color to white and background to black
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  
  // Set text datum to Middle Center (MC_DATUM)
  // This means the coordinates (x,y) for tft.drawString will be the center of the text
  tft.setTextDatum(MC_DATUM);

  // Set the font (default is fine, or choose another loaded font)
  tft.setTextFont(4); // Font 4 is a nice medium size

  // Print "Hello World!" in the center of the screen
  // tft.width() and tft.height() give the dimensions of the screen
  tft.drawString("Hello World!", tft.width() / 2, tft.height() / 2);
  Serial.println("Text Drawn");

  // Optional: Turn on the backlight if it's controlled by TFT_BL and not always on
  // For TTGO T-Display, the backlight pin (GPIO 4) is usually controlled by the library
  // If you have issues, you might need to manually control it:
  // pinMode(TFT_BL, OUTPUT);
  // digitalWrite(TFT_BL, HIGH); // Turn backlight on
}

void loop() {
  // Keep the display showing the message.
  // You can add other code here to update the display later.
  delay(1000); // Delay to prevent the loop from running too fast (optional)
}