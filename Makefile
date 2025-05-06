CC = gcc
CFLAGS = -Wall -Wextra -Wformat -Wformat=2 -Wimplicit-fallthrough -Werror=format-security -D_GLIBCXX_ASSERTIONS -fstrict-flex-arrays=3 -fstack-clash-protection -fstack-protector-strong -ffunction-sections -fdata-sections
LDFLAGS = -lm -pthread -lrt -Wl,--gc-sections -s -Wl,-z,nodlopen -Wl,-z,noexecstack -Wl,-z,relro -Wl,-z,now -Wl,--as-needed -Wl,--no-copy-dt-needed-entries
LDFLAGS += -L/usr/local/lib -lraylib -lm -lpthread -lrt -ljson-c

BUILD_DIR = build
SRC_DIR = src

TARGETS = central_control_system zone_controller wayside_equipment train cbtc_orchestrator

all: $(TARGETS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

central_control_system: src/central_control_system.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/$@ $< $(LDFLAGS)

zone_controller: src/zone_controller.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/$@ $< $(LDFLAGS)

wayside_equipment: src/wayside_equipment.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/$@ $< $(LDFLAGS)

train: src/train.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/$@ $< $(LDFLAGS)

cbtc_orchestrator: src/cbtc_orchestrator.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/$@ $< $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR)

debug: CFLAGS += -ggdb -O -DDEBUG -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 
debug: all

release: CFLAGS += -O3 -DNDEBUG
release: all

run: all
	cd $(BUILD_DIR) && ./cbtc_orchestrator

help:
	@echo "CBTC System Makefile"
	@echo "Available targets:"
	@echo "  all         - Build all components (default)"
	@echo "  clean       - Remove build artifacts"
	@echo "  debug       - Build with debug symbols"
	@echo "  release     - Build optimized version"
	@echo "  run         - Build and run the orchestrator"
	@echo ""
	@echo "Individual components:"
	@echo "  central_control_system"
	@echo "  zone_controller"
	@echo "  wayside_equipment"
	@echo "  train"
	@echo "  cbtc_orchestrator"

.PHONY: all clean debug release run help
