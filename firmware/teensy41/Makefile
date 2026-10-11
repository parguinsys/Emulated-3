# Compile check without PlatformIO / Teensyduino: builds against a checkout of PaulStoffregen/cores and /SPI with
# arm-none-eabi-gcc.  For uploading use PlatformIO (platformio.ini) or the Arduino IDE with Teensyduino.
#   make TEENSY_CORE=/path/to/cores/teensy4 TEENSY_SPI=/path/to/SPI
TEENSY_CORE ?= ../../../teensy-cores/teensy4
TEENSY_SPI  ?= ../../../teensy-spi
BUILD       ?= build
CC  = arm-none-eabi-gcc
CXX = arm-none-eabi-g++
CPU = -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-d16 -mthumb
DEFS = -D__IMXRT1062__ -DTEENSYDUINO=159 -DARDUINO=10819 -DARDUINO_TEENSY41 -DF_CPU=600000000 -DUSB_SERIAL -DLAYOUT_US_ENGLISH
FLAGS = $(CPU) $(DEFS) -O2 -g -Wall -ffunction-sections -fdata-sections -MMD -I$(TEENSY_CORE) -I$(TEENSY_SPI)
CXXFLAGS = $(FLAGS) -std=gnu++17 -fno-exceptions -fpermissive -fno-rtti -fno-threadsafe-statics -felide-constructors -Wno-error=narrowing
CFLAGS = $(FLAGS)
CORE_C   = $(wildcard $(TEENSY_CORE)/*.c)
CORE_CPP = $(filter-out $(TEENSY_CORE)/main.cpp,$(wildcard $(TEENSY_CORE)/*.cpp))
APP      = src/firmware.cpp src/board_logic.cpp src/board_link.cpp $(TEENSY_CORE)/main.cpp $(TEENSY_SPI)/SPI.cpp
OBJ      = $(addprefix $(BUILD)/core/,$(notdir $(CORE_C:.c=.o) $(CORE_CPP:.cpp=.o))) $(addprefix $(BUILD)/app/,$(notdir $(APP:.cpp=.o)))
vpath %.c $(TEENSY_CORE)
vpath %.cpp src $(TEENSY_CORE) $(TEENSY_SPI)

$(BUILD)/eiii_board.hex: $(BUILD)/eiii_board.elf
	arm-none-eabi-objcopy -O ihex -R .eeprom $< $@
	arm-none-eabi-size $<
$(BUILD)/eiii_board.elf: $(OBJ)
	$(CXX) $(CPU) -O2 --specs=nano.specs -Wl,--gc-sections,--relax -T$(TEENSY_CORE)/imxrt1062_t41.ld $^ -lm -lstdc++ -o $@
$(BUILD)/core/%.o: %.c | $(BUILD)/core
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/core/%.o: %.cpp | $(BUILD)/core
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(BUILD)/app/%.o: %.cpp | $(BUILD)/app
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(BUILD)/core $(BUILD)/app:
	mkdir -p $@
clean:
	rm -rf $(BUILD)
-include $(OBJ:.o=.d)
.PHONY: clean
