ROM ?= baserom.nds
OUTPUT_ROOT ?= output
ROM_STEM := $(basename $(notdir $(ROM)))
ROM_OUTPUT ?= $(OUTPUT_ROOT)/$(ROM_STEM)
NATIVE_DIR ?= $(ROM_OUTPUT)/native
INSTRUCTIONS ?= 0
PYTHON ?= python3
CMAKE ?= cmake

DEVKIT_IMAGE := devkitpro/devkitarm:20260610
PROJECT_DIR := $(abspath .)
DOCKER_RUN := docker run --rm --user $(shell id -u):$(shell id -g) -v $(PROJECT_DIR):/work -w /work $(DEVKIT_IMAGE)

DSD := .tools/bin/dsd
OBJDIFF := .tools/bin/objdiff
DSD_GHIDRA := .tools/ghidra/dsd-ghidra.zip
DECOMP_DIR := $(ROM_OUTPUT)/decomp
DSD_CONFIG := $(DECOMP_DIR)/config/arm9/config.yaml

.PHONY: all check check-windows clean decomp-init distclean ds ds-clean ghidra native-linux native-windows objdiff project rom-check setup tools versions

all: native-linux

project:
	$(PYTHON) tools/ndsrecomp.py create "$(ROM)" --output "$(NATIVE_DIR)" --instructions "$(INSTRUCTIONS)"

native-linux: project
	$(CMAKE) -S "$(NATIVE_DIR)" -B "$(NATIVE_DIR)/build-linux" -G Ninja -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build "$(NATIVE_DIR)/build-linux"

native-windows: project
	$(CMAKE) -S "$(NATIVE_DIR)" -B "$(NATIVE_DIR)/build-windows" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$(abspath $(NATIVE_DIR))/cmake/mingw-w64.cmake"
	$(CMAKE) --build "$(NATIVE_DIR)/build-windows"

check: native-linux
	$(PYTHON) tools/ndsrecomp.py self-test
	"$(NATIVE_DIR)/build-linux/ndsrecomp" --self-test

check-windows: native-windows
	wine "$(NATIVE_DIR)/build-windows/ndsrecomp.exe" --self-test

clean:
	$(CMAKE) -E remove_directory "$(NATIVE_DIR)/build-linux"
	$(CMAKE) -E remove_directory "$(NATIVE_DIR)/build-windows"

distclean:
	$(CMAKE) -E remove_directory "$(NATIVE_DIR)"

ds:
	$(DOCKER_RUN) make -f Makefile.libnds OUTPUT_DIR="$(ROM_OUTPUT)/ds"

ds-clean:
	$(DOCKER_RUN) make -f Makefile.libnds OUTPUT_DIR="$(ROM_OUTPUT)/ds" clean

setup: tools
	docker pull $(DEVKIT_IMAGE)

tools: $(DSD) $(OBJDIFF) $(DSD_GHIDRA)

$(DSD):
	mkdir -p $(@D)
	curl -fL --retry 3 -o $@ https://github.com/AetiasHax/ds-decomp/releases/download/v0.11.0/dsd-linux-x86_64
	echo '916f5474498272e3ecbaa25ea029cf58d6f2df5b212a21956748e19e49f4d64e  $@' | sha256sum -c -
	chmod +x $@

$(OBJDIFF):
	mkdir -p $(@D)
	curl -fL --retry 3 -o $@ https://github.com/encounter/objdiff/releases/download/v3.7.3/objdiff-linux-x86_64
	echo 'd69ce1cc9525fc12503f65d2df7dca21f931d58622f19c9174f32fe4c9c91b20  $@' | sha256sum -c -
	chmod +x $@

$(DSD_GHIDRA):
	mkdir -p $(@D)
	curl -fL --retry 3 -o $@ https://github.com/AetiasHax/dsd-ghidra/releases/download/v0.6.0/ghidra_11.2.1_PUBLIC_20260410_dsd-ghidra.zip
	echo '2966467e79f6e3638c8689aea7d2f59349ec9ba15f8fdd54d51f3466587ff57f  $@' | sha256sum -c -

rom-check:
	$(PYTHON) tools/ndsrecomp.py inspect "$(ROM)" >/dev/null

decomp-init: $(DSD) rom-check
	@unit=$$(od -An -tu1 -j18 -N1 "$(ROM)" | tr -d ' '); \
	if [ "$$unit" != 0 ]; then \
		echo 'DSD 0.11.0 cannot initialize DSi-enhanced ROMs (unit code != 0).'; \
		echo 'Track upstream support: https://github.com/AetiasHax/ds-rom/issues/11'; \
		exit 2; \
	fi
	mkdir -p "$(DECOMP_DIR)/extract" "$(DECOMP_DIR)/config" "$(DECOMP_DIR)/build/orig"
	$(DSD) rom extract --rom "$(ROM)" --output-path "$(DECOMP_DIR)/extract"
	$(DSD) init --rom-config "$(DECOMP_DIR)/extract/config.yaml" --output-path "$(DECOMP_DIR)/config" --build-path "$(DECOMP_DIR)/build/orig"
	$(DSD) delink --config-path "$(DSD_CONFIG)"
	$(DSD) objdiff --config-path "$(DSD_CONFIG)" --output-path "$(DECOMP_DIR)"

objdiff: $(OBJDIFF)
	$(OBJDIFF) --project-dir "$(DECOMP_DIR)"

ghidra:
	test -x "$${GHIDRA_HOME:-/opt/ghidra}/ghidraRun"
	"$${GHIDRA_HOME:-/opt/ghidra}/ghidraRun"

versions: tools
	$(DSD) --version
	$(OBJDIFF) --version
	docker run --rm $(DEVKIT_IMAGE) sh -c '"$$DEVKITARM/bin/arm-none-eabi-gcc" --version | head -1'
