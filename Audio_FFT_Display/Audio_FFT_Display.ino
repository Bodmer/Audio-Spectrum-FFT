// Display the audio frequency spectrum gathered from the PDM microphone on a
// RP2040 Nano Connect board.

// Uses the ApproxFFT functions from here(see sketch tab):
// https://www.instructables.com/ApproxFFT-Fastest-FFT-Function-for-Arduino/

// RP2040 SPI TFT 62.5MHz SPI clock, typical fps to draw spectrum and peak:
// FFT samples = 64  -> 100 fps
// FFT samples = 128 ->  78 fps
// FFT samples = 256 ->  31 fps
// FFT samples = 512 ->  15 fps

// Requires the Earle Philhower RP2040 board package (includes PDM microphone library):
// https://github.com/earlephilhower/arduino-pico

// Uses the TFT_eSPI library here:
// https://github.com/Bodmer/TFT_eSPI

// Note: the PDM library uses the RP2040 PIO capability and will not work if TFT_eSPI
// is also configured to use PIO for the TFT interface. So this only works with the
// standard RP2040 SPI interface. Run processor clock at 125MHz.

#include <TFT_eSPI.h>                 // Include the graphics library (this includes the sprite functions)
#include <PDM.h>

// If the samples are set to N then the FFT generates N/2 valid points
#define SAMPLES 256 //Number of samples for the FFT - 64, 128, 256 or 512
#define SAMPLING_FREQUENCY 10000 // Frequency range plotted is 2.5kHz (sampling frequency/4)

// Display options
#define DRAW_SPECTRUM  // Draw the spectrum
#define DRAW_PEAK      // Draw spectrum peak (if DRAW_SPECTRUM defined)
//#define DRAW_WATERFALL // Draw a frequency waterfall
#define DRAW_TRACE     // Draw a scope type trace (not good with waterfall!)

// Scale factors for the display
#define TRACE_SCALE 25      // Scale factor for 'scope trace amplitude
#define FFT_SCALE  SAMPLES  // Scale factor for FFT bar amplitude (scale = samples seems to work)
#define EXP_FILTER 0.95     // Exponential peak filter decay rate, 0.9 fast decay, 0.99 very slow

// Audio sample buffer
short sampleBuffer[64];
short dumpBuffer[64];

// Audio sample buffers used for analysis and display
short streamBuffer[SAMPLES]; // Sample stream buffer (N samples)
int approxBuffer[SAMPLES];   // ApproxFFT sample buffer
int peakBuffer[SAMPLES];     // Amplitude peak buffer

volatile int samplesRead = 0; // Samples read from microphone (expected to be 64)
volatile int sampleCount = 0; // Samples put into streamBuffer

uint16_t counter = 0; // Frame counter
long startMillis = millis(); // Timer for FPS
uint16_t interval = 100; // FPS calculated over 100 frames
String fps = "0 fps";

void onPDMdata(void);  // Call back to get the audio samples

TFT_eSPI    tft = TFT_eSPI();         // Declare object "tft"

TFT_eSprite spr = TFT_eSprite(&tft);  // Declare Sprite object "spr" with pointer to "tft" object

// Sprite width and height
#define WIDTH  256
#define HEIGHT 128

// Pointer to the sprite image
uint16_t* sptr = nullptr;


void setup() {
  // Serial used for debug and test (fps report) only
  Serial.begin(115200);
  //while (!Serial);

  // Initialise the TFT library
  tft.init();
  tft.setRotation(1);
  tft.initDMA();
  tft.fillScreen(TFT_NAVY);
  tft.startWrite();

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("Audio spectrum FFT", tft.width()/2, 5, 4);

  String str = String((int)SAMPLES) + " samples";
  tft.drawString(str, tft.width()/2, 30, 2);

  tft.setTextDatum(TL_DATUM);
  str = "Max: " + String((int)SAMPLING_FREQUENCY/4000.0) + "kHz";
  tft.drawString(str, 31, 55 + HEIGHT + 5, 2);

  // Create the sprite and get the image pointer
  sptr = (uint16_t*)spr.createSprite(WIDTH, HEIGHT);

  // Initialise the PDM library
  PDM.onReceive(onPDMdata);
  PDM.setGain(20);
  // setup the I2S audio input for the sample rate with 32-bits per sample
  if (!PDM.begin(1, SAMPLING_FREQUENCY)) {
    Serial.println("Failed to start PDM!");
    while (1);
  }

  // Reset FPS timer
  startMillis = millis();
}

void loop() {
  // Only 64 PDM audio samples are available at a time so accumulate 256 for the FFT
  if (samplesRead) {
    // Fill the buffers with the samples
    for (int i = 0; i < samplesRead; i++) {
      streamBuffer[i + sampleCount] = sampleBuffer[i];
      approxBuffer[i + sampleCount] = sampleBuffer[i] / 2;
    }
    sampleCount += samplesRead;
    samplesRead = 0;
  }

  // If we have all new samples then run the FFT.
  if (sampleCount >= SAMPLES) {

    // Do the FFT number crunching
    // approxBuffer contains samples, but will be updated with amplitudes
    Approx_FFT(approxBuffer, SAMPLES, SAMPLING_FREQUENCY);

    // Wait for DMA to complete (probably FFT is slower)
    tft.dmaWait();

    // Pixel width of a frequency band
    uint16_t pw = (4.0 * WIDTH / SAMPLES);

#ifndef DRAW_WATERFALL
    spr.fillSprite(TFT_BLACK);
#endif

#ifdef DRAW_SPECTRUM
    for (uint16_t i = 0; i < SAMPLES / 4; i++) {
  #ifdef DRAW_PEAK
      // Track the peak and update decay filter
      if (approxBuffer[i] > peakBuffer[i]) {
        if (approxBuffer[i] / FFT_SCALE < HEIGHT) peakBuffer[i] = approxBuffer[i];
      }
      else peakBuffer[i] = peakBuffer[i] * EXP_FILTER + approxBuffer[i] * (1.0 - EXP_FILTER);
  #endif
      // Convert amplitude to y height, give higher frequencies a boost
      uint16_t hp = (1 + (i / 10.0)) * peakBuffer[i] / FFT_SCALE;
      uint16_t hr = (1 + (i / 10.0)) * approxBuffer[i] / FFT_SCALE;

      if (hr > HEIGHT) hr = HEIGHT;
  #ifdef DRAW_PEAK
      if (hp > HEIGHT) hp = HEIGHT;
      uint16_t col = rainbowColor(127 + min(hp, 96));
      spr.fillRect(pw * i, HEIGHT - hp, pw - 1, hp - hr, col);
  #endif
      spr.fillRect(pw * i, HEIGHT - hr, pw - 1, hr, TFT_WHITE);
    }
#endif

#ifdef DRAW_TRACE
    // Find a trigger point in the first half of the buffer to stabilise the trace start
    uint16_t startSample = 0;
    while (startSample < SAMPLES / 2 - 1) {
      // Look for a rising edge near zero
      if (streamBuffer[startSample] > TRACE_SCALE && streamBuffer[startSample] < 4 * TRACE_SCALE && streamBuffer[startSample + 1] > streamBuffer[startSample]) break;
      startSample++;
    }

    // Render the 'scope trace, only a half the buffer is plotted after the trigger point
    for (uint16_t x = 0; x < WIDTH - pw; x += pw) {
      spr.drawLine(x, HEIGHT / 2 - (streamBuffer[startSample] / TRACE_SCALE), x + pw, HEIGHT / 2 - (streamBuffer[startSample + 1] / TRACE_SCALE), TFT_GREEN);
      startSample++;
      if (startSample >= SAMPLES - 1) break;
    }
#endif

#ifdef DRAW_WATERFALL
    // FFT waterfall
    spr.scroll(0, 1); // Scroll sprite down 1 pixel
    for (uint16_t i = 0; i < SAMPLES / 4; i++) {
      // Conver to y height, give higher frequencies a boost
      uint16_t hr = (1 + (i / 10.0)) * approxBuffer[i] / (FFT_SCALE / 2);
      if (hr > 127) hr = 127;
      uint16_t col = rainbowColor(128 - hr);
      spr.drawFastHLine(pw * i, 0, pw, col);
    }
#endif

    // Can use pushImage instead, but DMA provides better performance
    // tft.pushImage(31, 55, WIDTH, HEIGHT, sptr);
    tft.pushImageDMA(31, 55, WIDTH, HEIGHT, sptr);

    sampleCount = 0;

    counter++;
    // only calculate the fps every <interval> iterations.
    if (counter % interval == 0) {
      long millisSinceUpdate = millis() - startMillis;
      fps = String((int)(interval * 1000.0 / (millisSinceUpdate))) + " fps";
      Serial.println(fps);
      tft.setTextDatum(TR_DATUM);
      tft.setTextPadding(tft.textWidth(" 999 fps ", 2));
      tft.drawString(fps, 31 + WIDTH, 55 + HEIGHT + 5, 2);
      startMillis = millis();
    }
    // Been a while, so reset to synch with next clean buffer
    samplesRead = 0;
  }
}

void onPDMdata()
{
  // This fetches a maximum of 64 samples
  int bytesAvailable = PDM.available();
  PDM.read(sampleBuffer, bytesAvailable);
  samplesRead = bytesAvailable / 2;
}

// If 'spectrum' is in the range 0-159 it is converted to a spectrum colour
// from 0 = red through to 127 = blue to 159 = violet
// Extending the range to 0-191 adds a further violet to red band
uint16_t rainbowColor(uint8_t spectrum)
{
  spectrum = spectrum % 192;

  uint8_t red   = 0; // Red is the top 5 bits of a 16 bit colour spectrum
  uint8_t green = 0; // Green is the middle 6 bits, but only top 5 bits used here
  uint8_t blue  = 0; // Blue is the bottom 5 bits

  uint8_t sector = spectrum >> 5;
  uint8_t amplit = spectrum & 0x1F;

  switch (sector)
  {
    case 0:
      red   = 0x1F;
      green = amplit; // Green ramps up
      blue  = 0;
      break;
    case 1:
      red   = 0x1F - amplit; // Red ramps down
      green = 0x1F;
      blue  = 0;
      break;
    case 2:
      red   = 0;
      green = 0x1F;
      blue  = amplit; // Blue ramps up
      break;
    case 3:
      red   = 0;
      green = 0x1F - amplit; // Green ramps down
      blue  = 0x1F;
      break;
    case 4:
      red   = amplit; // Red ramps up
      green = 0;
      blue  = 0x1F;
      break;
    case 5:
      red   = 0x1F;
      green = 0;
      blue  = 0x1F - amplit; // Blue ramps down
      break;
  }

  return red << 11 | green << 6 | blue;
}
