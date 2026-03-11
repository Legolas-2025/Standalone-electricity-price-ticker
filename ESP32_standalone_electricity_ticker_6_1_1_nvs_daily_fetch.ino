void setWhiteLedOff() {
    digitalWrite(whiteLedPin, LOW);
    analogWrite(whiteLedPin, 0);
}

void updateLeds() {
    // Existing LED logic...
    // Early return for certain conditions may call setWhiteLedOff()
    if (/* Some condition */) {
        setWhiteLedOff();
        return;
    }
    // Logic to handle switching between states...
    if (lastLedMode == LED_MODE_DIGITAL) {
        analogWrite(whiteLedPin, 0); // Detach PWM
    }
    ledcAttachPin(whiteLedPin, /* channel */);
    // Handle other operations
    if (//condition for breathing mode) {
        analogWrite(whiteLedPin, value);
    } //additional logic
}

void setup() {
    pinMode(whiteLedPin, OUTPUT);
    digitalWrite(whiteLedPin, LOW); // Fix initial state
    // Other setup code...
}
