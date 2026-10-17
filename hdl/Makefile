# HDL for the FPGA voice engine.  `make sim` builds the Verilator testbench; `make test` runs the random comparison
# against the C++ F-Chip model.  `make trace TRACES=../../analysis/traces` also replays the MAME traces.
VERILATOR ?= verilator
TRACES    ?= ../../analysis/traces
RTL       = fchip/fchip_lane.sv fchip/fchip_bank.sv
OBJ       = build/obj

all: sim

lint:
	$(VERILATOR) --lint-only -Wall --top-module fchip_bank $(RTL)

sim: build/tb_fchip

build/tb_fchip: $(RTL) sim/tb_fchip.cpp ../fchip/eiii_fchip.cpp ../fchip/eiii_fchip.h
	mkdir -p $(OBJ)
	$(VERILATOR) -Wall --cc --exe --build -O3 --top-module fchip_bank -Mdir $(OBJ) -o ../tb_fchip \
		-CFLAGS "-std=c++17 -O2 -I$(abspath ../fchip)" $(RTL) $(abspath sim/tb_fchip.cpp ../fchip/eiii_fchip.cpp)

test: lint build/tb_fchip
	for s in 1 2 3 4 5; do ./build/tb_fchip random $$s 20000 || exit 1; done

trace: build/tb_fchip
	for t in fwdbk oneshot_loop dpscrub; do ./build/tb_fchip $(TRACES)/fchip_equiv/$$t.trace $(TRACES)/fchip_equiv/$$t.snd || exit 1; done

clean:
	rm -rf build

.PHONY: all lint sim test trace clean
