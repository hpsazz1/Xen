#include <USBHost_t36.h>

#if !defined(MOUSE_INTERFACE)
#error "Select Tools > USB Type > Serial + Keyboard + Mouse + Joystick"
#endif

#if !defined(CDC_STATUS_INTERFACE)
#error "Select a USB Type that includes Serial, such as Serial + Keyboard + Mouse + Joystick"
#endif

USBHost myusb;
USBHub hub1(myusb);
USBHub hub2(myusb);
USBHIDParser hid1(myusb);
USBHIDParser hid2(myusb);
USBHIDParser hid3(myusb);
USBHIDParser hid4(myusb);
MouseController mouse1(myusb);

constexpr bool DEBUG_HOST_MOUSE = false;

uint8_t hostButtons = 0;
uint8_t serialButtons = 0;

int8_t clampMouseStep(int value) {
  if (value > 127) return 127;
  if (value < -127) return -127;
  return (int8_t)value;
}

void moveMouseInSteps(int dx, int dy, int wheel, int wheelH) {
  while (dx != 0 || dy != 0 || wheel != 0 || wheelH != 0) {
    int8_t stepX = clampMouseStep(dx);
    int8_t stepY = clampMouseStep(dy);
    int8_t stepWheel = clampMouseStep(wheel);
    int8_t stepWheelH = clampMouseStep(wheelH);

    Mouse.move(stepX, stepY, stepWheel, stepWheelH);

    dx -= stepX;
    dy -= stepY;
    wheel -= stepWheel;
    wheelH -= stepWheelH;
  }
}

void setMouseButtons(uint8_t buttons) {
  Mouse.set_buttons(
    buttons & MOUSE_LEFT,
    buttons & MOUSE_MIDDLE,
    buttons & MOUSE_RIGHT,
    buttons & MOUSE_BACK,
    buttons & MOUSE_FORWARD
  );
}

void applyButtons() {
  setMouseButtons(hostButtons | serialButtons);
}

uint8_t buttonIdForMask(uint8_t mask) {
  if (mask == MOUSE_LEFT) return 1;
  if (mask == MOUSE_RIGHT) return 2;
  if (mask == MOUSE_MIDDLE) return 3;
  if (mask == MOUSE_BACK) return 4;
  if (mask == MOUSE_FORWARD) return 5;
  return 0;
}

void emitButtonTransition(uint8_t previousButtons, uint8_t currentButtons, uint8_t mask) {
  bool wasDown = (previousButtons & mask) != 0;
  bool isDown = (currentButtons & mask) != 0;
  if (wasDown == isDown) return;

  uint8_t buttonId = buttonIdForMask(mask);
  if (buttonId == 0) return;

  if (isDown) {
    Serial.printf("BD:%u\n", buttonId);
  } else {
    Serial.printf("BU:%u\n", buttonId);
  }
}

void emitButtonTransitions(uint8_t previousButtons, uint8_t currentButtons) {
  emitButtonTransition(previousButtons, currentButtons, MOUSE_LEFT);
  emitButtonTransition(previousButtons, currentButtons, MOUSE_RIGHT);
  emitButtonTransition(previousButtons, currentButtons, MOUSE_MIDDLE);
  emitButtonTransition(previousButtons, currentButtons, MOUSE_BACK);
  emitButtonTransition(previousButtons, currentButtons, MOUSE_FORWARD);
}

void handleSerialCommand(char *line) {
  int dx = 0;
  int dy = 0;
  int wheel = 0;
  int wheelH = 0;
  int buttons = 0;

  if (strcmp(line, "ping") == 0) {
    Serial.println("pong");
  } else if (strcmp(line, "status") == 0) {
    Serial.println("usb=serial+mouse host=ready protocol=teensy41");
  } else if (sscanf(line, "move %d %d %d %d", &dx, &dy, &wheel, &wheelH) >= 2) {
    moveMouseInSteps(dx, dy, wheel, wheelH);
  } else if (sscanf(line, "buttons %d", &buttons) == 1) {
    serialButtons = (uint8_t)buttons;
    applyButtons();
    Serial.println("ok");
  } else {
    Serial.println("commands: ping, status, move dx dy [wheel] [wheelH], buttons mask");
  }
}

void readSerialCommands() {
  static char line[96];
  static uint8_t length = 0;

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;

    if (c == '\n') {
      line[length] = 0;
      if (length > 0) handleSerialCommand(line);
      length = 0;
    } else if (length < sizeof(line) - 1) {
      line[length++] = c;
    } else {
      length = 0;
      Serial.println("serial line too long");
    }
  }
}

void forwardHostMouse() {
  if (!mouse1.available()) return;

  int dx = mouse1.getMouseX();
  int dy = mouse1.getMouseY();
  int wheel = mouse1.getWheel();
  int wheelH = mouse1.getWheelH();
  uint8_t buttons = mouse1.getButtons();

  moveMouseInSteps(dx, dy, wheel, wheelH);

  if (buttons != hostButtons) {
    emitButtonTransitions(hostButtons, buttons);
    hostButtons = buttons;
    applyButtons();
  }

  if (DEBUG_HOST_MOUSE) {
    Serial.printf("host mouse: dx=%d dy=%d wheel=%d wheelH=%d buttons=%u\n",
                  dx, dy, wheel, wheelH, buttons);
  }

  mouse1.mouseDataClear();
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);
  delay(500);

  myusb.begin();
  Mouse.begin();

  Serial.println("Teensy 4.1 mouse serial bridge ready");
  Serial.println("Plug the physical mouse into the Teensy 4.1 USB host port.");
}

void loop() {
  myusb.Task();
  forwardHostMouse();
  readSerialCommands();
}
