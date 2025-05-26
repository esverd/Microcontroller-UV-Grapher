#include <Arduino.h>
#include <SPI.h>      // Required for TFT_eSPI
#include <TFT_eSPI.h> // Include the TFT_eSPI library

// Create an instance of the TFT_eSPI class
TFT_eSPI tft = TFT_eSPI();

void setup() {
  Serial.begin(115200);
  Serial.println("\nHello World! Diagnostic Test...");

  tft.init();
  Serial.println("TFT Initialized.");

  // Set rotation (e.g., 0 for portrait, 1 for landscape)
  // The tft.width() and tft.height() will change based on this.
  tft.setRotation(0); // << Try 0 first. Then maybe 1, 2, 3 to see if behavior changes.
  Serial.print("Screen rotation set to: "); Serial.println(tft.getRotation());

  // --- Diagnostic Prints ---
  // These will tell us what dimensions the library *thinks* the screen has.
  int configured_width = tft.width();
  int configured_height = tft.height();
  Serial.print("TFT configured width: "); Serial.println(configured_width);
  Serial.print("TFT configured height: "); Serial.println(configured_height);

  // --- Visual Boundary Test ---
  // Fill the screen with a color. Does this color fill your *entire physical screen*?
  tft.fillScreen(TFT_BLUE); // Use a bright color like BLUE or RED
  Serial.println("Screen filled with BLUE. Check if it covers the entire physical display.");
  delay(2000); // Pause for 2 seconds to observe the fill

  // Draw a border at the edges of the configured area
  // If this border is smaller than your physical screen, the configured dimensions are too small.
  tft.drawRect(0, 0, configured_width, configured_height, TFT_WHITE);
  Serial.println("White rectangle drawn at configured boundaries (0,0 to width-1, height-1).");
  delay(2000); // Pause

  // Draw some lines to further check boundaries
  tft.drawLine(0,0, configured_width -1, configured_height -1, TFT_GREEN); // Top-left to bottom-right
  tft.drawLine(configured_width -1, 0, 0, configured_height -1, TFT_RED);   // Top-right to bottom-left
  delay(2000);


  // --- Display "Hello World!" ---
  tft.setTextColor(TFT_YELLOW, TFT_BLUE); // Yellow text on the blue background
  tft.setTextDatum(MC_DATUM); // Middle Center datum
  tft.setTextFont(4);         // Font 4

  // Display "Hello World!" in the center of the *configured* screen area
  tft.drawString("Hello World!", configured_width / 2, configured_height / 2);
  Serial.println("'Hello World!' drawn.");

  // Also print the dimensions on screen
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_BLUE);
  String dims = String(configured_width) + "x" + String(configured_height);
  tft.drawString(dims, configured_width / 2, configured_height / 2 + 30); // Below "Hello World"

  Serial.println("Setup complete. Observe the display.");
}

void loop() {
  delay(1000);
}