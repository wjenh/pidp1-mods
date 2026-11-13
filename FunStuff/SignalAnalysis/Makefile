PDP1_ROOT ?= /opt/pidp1-mods
AM1 ?= $(PDP1_ROOT)/bin/am1
FS_DEPS := $(wildcard $(PDP1_ROOT)/Am1Includes/FILESYSTEM/*.ah $(PDP1_ROOT)/Am1Includes/FILESYSTEM/*.ac $(PDP1_ROOT)/Am1Includes/MICROTAPE/*.ah $(PDP1_ROOT)/Am1Includes/MICROTAPE/*.ac)

.PHONY: all check run live-status tables drum-status drum-image drum-changing drum-pick drum-restore
all: workstation.rim workstation-tape.rim

workstation.rim: workstation.am1 state.ac ui-state.ac kernel.ac frontend.ac display.ac tape-loader.ac $(FS_DEPS) generated/tape-config.ac generated/twiddles.ac generated/bitrev.ac generated/scene.ac
	$(AM1) -d -S workstation.am1

generated/twiddles.ac generated/bitrev.ac generated/scene.ac &: generate_tables.py generated/tape-config.json
	python3 generate_tables.py

tables:
	python3 generate_tables.py

check:
	PDP1_ROOT="$(PDP1_ROOT)" PYTHONDONTWRITEBYTECODE=1 python3 verify_package.py

run: workstation.rim
	python3 live.py --root "$(PDP1_ROOT)" run

live-status:
	python3 live.py --root "$(PDP1_ROOT)" status

drum-status:
	python3 drum.py --root "$(PDP1_ROOT)" status

drum-image:
	python3 drum.py --root "$(PDP1_ROOT)" create

drum-changing:
	python3 drum.py --root "$(PDP1_ROOT)" create-changing

drum-pick:
	python3 drum.py --root "$(PDP1_ROOT)" pick

drum-restore:
	python3 drum.py --root "$(PDP1_ROOT)" restore

workstation-tape.rim: workstation.am1 state.ac ui-state.ac kernel.ac frontend.ac display.ac tape-loader.ac $(FS_DEPS) generated/tape-config.ac generated/twiddles.ac generated/bitrev.ac generated/scene.ac
	cp workstation.am1 workstation-tape.am1
	$(AM1) -d -S -DSAW_TAPE workstation-tape.am1

.PHONY: run-tape
run-tape: workstation.rim workstation-tape.rim
	python3 live.py --root "$(PDP1_ROOT)" --source tape run

.PHONY: test manifest
test:
	$(MAKE) -f Makefile.tests test

manifest:
	python3 update_manifest.py

# Keep shipped preassembled programs and generated tables for offline operation.
# Remove only optional test products and assembler listings/maps.
.PHONY: clean
clean:
	rm -f *.lst *.map core.rim core.sym probe.rim probe.sym
	rm -f workstation-tape.am1 workstation-test.* workstation-sequence.* workstation-wav-test.* workstation-tape-test.* workstation-tape-sequence.*
	rm -f tests/cpu_runner tests/frontend_runner tests/wav_runner tests/tape_runner
