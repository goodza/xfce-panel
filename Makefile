PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib
DATADIR ?= $(PREFIX)/share
BINDIR ?= $(PREFIX)/bin
DESTDIR ?=

CARGO ?= cargo
PKG_CONFIG ?= pkg-config
INSTALL ?= install
CARGO_TARGET_DIR ?= target/native
NATIVE_BUILD_DIR := $(CARGO_TARGET_DIR)/panel
RUST_LIB := $(CARGO_TARGET_DIR)/release/libxfce_rust_monitor.a
CLI := $(CARGO_TARGET_DIR)/release/rust-monitor
PLUGIN := $(NATIVE_BUILD_DIR)/librust-monitor.so
FAN_TEST_PLUGIN := $(NATIVE_BUILD_DIR)/librust-monitor-fan-test.so
PACKAGES := libxfce4panel-2.0 gtk+-3.0 gio-2.0
PLUGIN_LIBDIR := $(LIBDIR)/xfce4/panel/plugins
PLUGIN_DATADIR := $(DATADIR)/xfce4/panel/plugins

.PHONY: all check-deps rust install uninstall clean test test-ffi test-native test-embedded
all: check-deps $(PLUGIN)

check-deps:
	@command -v $(PKG_CONFIG) >/dev/null 2>&1 || { echo "Error: pkg-config is required to build the native panel plugin." >&2; exit 1; }
	@$(PKG_CONFIG) --exists $(PACKAGES) || { echo "Error: native development dependencies missing: $(PACKAGES). Install their development headers to build the panel plugin." >&2; $(PKG_CONFIG) --print-errors --exists $(PACKAGES); exit 1; }
	@$(PKG_CONFIG) --atleast-version=4.19 libxfce4panel-2.0 || { echo "Error: XFCE panel 4.19 or newer is required for popup-window support." >&2; exit 1; }

rust:
	RUSTFLAGS="$(RUSTFLAGS) -C relocation-model=pic" CARGO_TARGET_DIR="$(CARGO_TARGET_DIR)" $(CARGO) build --release --lib --bin rust-monitor

$(PLUGIN): native/rust-monitor.c include/rust-monitor.h rust | check-deps
	mkdir -p "$(NATIVE_BUILD_DIR)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -fPIC -Iinclude \
		$$($(PKG_CONFIG) --cflags $(PACKAGES)) -shared -o "$@" native/rust-monitor.c \
		"$(RUST_LIB)" $(LDFLAGS) -Wl,-z,nodelete \
		$$($(PKG_CONFIG) --libs $(PACKAGES)) -ldl -lpthread -lm $(LDLIBS)

install: all
	$(INSTALL) -d "$(DESTDIR)$(PLUGIN_LIBDIR)" "$(DESTDIR)$(PLUGIN_DATADIR)" "$(DESTDIR)$(BINDIR)"
	$(INSTALL) -m 755 "$(PLUGIN)" "$(DESTDIR)$(PLUGIN_LIBDIR)/librust-monitor.so"
	$(INSTALL) -m 644 data/rust-monitor.desktop "$(DESTDIR)$(PLUGIN_DATADIR)/rust-monitor.desktop"
	$(INSTALL) -m 755 "$(CLI)" "$(DESTDIR)$(BINDIR)/rust-monitor"

uninstall:
	rm -f "$(DESTDIR)$(PLUGIN_LIBDIR)/librust-monitor.so" \
		"$(DESTDIR)$(PLUGIN_DATADIR)/rust-monitor.desktop" \
		"$(DESTDIR)$(BINDIR)/rust-monitor"

test:
	$(CARGO) fmt --all -- --check
	$(CARGO) test --all-targets
	$(CARGO) clippy --all-targets -- -D warnings

test-ffi: rust
	mkdir -p "$(NATIVE_BUILD_DIR)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -Iinclude \
		tests/ffi-smoke.c "$(RUST_LIB)" $(LDFLAGS) -ldl -lpthread -lm $(LDLIBS) \
		-o "$(NATIVE_BUILD_DIR)/ffi-smoke"
	"$(NATIVE_BUILD_DIR)/ffi-smoke" config.example.toml

$(FAN_TEST_PLUGIN): native/rust-monitor.c include/rust-monitor.h tests/fan-stub.c rust | check-deps
	mkdir -p "$(NATIVE_BUILD_DIR)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC -Iinclude \
		-Dmonitor_fan_step=smoke_fan_step -Dmonitor_fan_result_free=smoke_fan_result_free \
		$$($(PKG_CONFIG) --cflags $(PACKAGES)) -shared -o "$@" native/rust-monitor.c tests/fan-stub.c \
		"$(RUST_LIB)" $(LDFLAGS) -Wl,-z,nodelete \
		$$($(PKG_CONFIG) --libs $(PACKAGES)) -ldl -lpthread -lm $(LDLIBS)

$(NATIVE_BUILD_DIR)/native-smoke: tests/native-smoke.c | check-deps
	mkdir -p "$(NATIVE_BUILD_DIR)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		$$($(PKG_CONFIG) --cflags $(PACKAGES) gmodule-2.0) \
		tests/native-smoke.c $(LDFLAGS) \
		$$($(PKG_CONFIG) --libs $(PACKAGES) gmodule-2.0) $(LDLIBS) \
		-o "$(NATIVE_BUILD_DIR)/native-smoke"

# Requires a working GTK display; unavailable displays are failures, not skips.
test-native: $(PLUGIN) $(FAN_TEST_PLUGIN) $(NATIVE_BUILD_DIR)/native-smoke
	"$(NATIVE_BUILD_DIR)/native-smoke" "$(abspath $(PLUGIN))"
	"$(NATIVE_BUILD_DIR)/native-smoke" "$(abspath $(FAN_TEST_PLUGIN))" --fan-stub
	RUST_MONITOR_TEST_NO_ANIMATIONS=1 "$(NATIVE_BUILD_DIR)/native-smoke" "$(abspath $(PLUGIN))"
	RUST_MONITOR_TEST_NO_ANIMATIONS=1 "$(NATIVE_BUILD_DIR)/native-smoke" "$(abspath $(FAN_TEST_PLUGIN))" --fan-stub

test-embedded: $(PLUGIN) $(NATIVE_BUILD_DIR)/native-smoke
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		$$($(PKG_CONFIG) --cflags $(PACKAGES)) tests/embedded-smoke.c \
		$$($(PKG_CONFIG) --libs $(PACKAGES)) $(LDFLAGS) $(LDLIBS) \
		-o "$(NATIVE_BUILD_DIR)/embedded-smoke"
	"$(NATIVE_BUILD_DIR)/embedded-smoke" "$(abspath $(NATIVE_BUILD_DIR)/native-smoke)" "$(abspath $(PLUGIN))"

clean:
	$(CARGO) clean --target-dir "$(CARGO_TARGET_DIR)"
