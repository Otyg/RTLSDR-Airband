#ifndef NOISE_REDUCTION_H
#define NOISE_REDUCTION_H

#include <cstdint>
#include <memory>

// Forward declaration for opaque RnnNoise state
struct DenoiseState;

class NoiseReduction {
   public:
    // Initialize noise reduction with pre-trained model
    // suppressionStrength: 0.0 (no suppression) to 1.0 (max suppression), recommended 0.7
    explicit NoiseReduction(float suppressionStrength = 0.7f);
    ~NoiseReduction();

    // Process an audio frame (10-40ms @ 16kHz recommended, typically 10ms = 160 samples)
    // Input: int16_t samples at 16kHz
    // Returns processed samples (same count as input)
    // Safe to call repeatedly with variable frame sizes
    void processFrame(int16_t* audioFrame, int frameSize);

    // Get/set suppression strength (0.0 to 1.0)
    float getSuppressionStrength() const { return suppressionStrength_; }
    void setSuppressionStrength(float strength);

    // Check if noise reduction is available (model loaded successfully)
    bool isInitialized() const { return state_ != nullptr; }

   private:
    std::unique_ptr<DenoiseState, void (*)(DenoiseState*)> state_;
    float suppressionStrength_;

    // Internal helper to load the pre-trained model
    bool initializeModel();
};

#endif
