/**
 * Plays audio through the Voice Chat.
 * 
 * @param clientIndex       In-game client that should play the audio
 * @param nSamples          Quantity of 16-bit Samples inside the pointer
 * @param data              Array of Samples
 */
native PlayAudioOnVoiceChat(int clientIndex, int nSamples, const char[] data);