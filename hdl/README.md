# FPGA voice engine (HDL)

SystemVerilog for the Cyclone V sampler platform (proposal: `/mnt/project-files/fpga/proposal_response.md` in the
project files). The first block is the EIII's digital voice hardware, so the original OS can drive FPGA voices.

* `fchip/fchip_lane.sv`: one EIII voice lane, a cycle-level port of `fchip/eiii_fchip.cpp`. That model is bit-exact
  against MAME (`tests/fchip_equiv`). It covers the 20 MHz / N rate divider, the register file (current/initial size and
  address, DEC / FORBK / LOOP flags), the F-Chip control-port byte pairs and a snapshot load port.
* `fchip/fchip_bank.sv`: 16 lanes, the read-back mux, and a sample fetch that reads every lane's current word for one
  output frame over a request/valid memory port.
* `sim/tb_fchip.cpp`: Verilator testbench.

## Running it

    make test                                 # lint + random comparison against the C++ model (5 seeds)
    make trace TRACES=/path/to/analysis/traces  # replay the MAME F-Chip traces (fwdbk, oneshot_loop, dpscrub)

The random test drives the HDL and the C++ model with the same register writes, control bytes and time gaps. It
compares all 64 register words after every event and all 16 lane outputs at every 96 kHz frame. Each seed runs about
50 M ticks.

## Known differences from the C++ model

Both come from how `advance(n)` handles time in large chunks. Neither should occur in normal OS use, and the random
test skips the events that would cause them (see the header of `fchip_lane.sv`):

1. A divisor write that leaves the divide count at or past twice the new divisor makes the model take several steps at
   once. The HDL takes one step per tick.
2. After a one-shot ends, the model's divide count depends on how the caller chunked time. The HDL stops it at 0. This
   shows only if a lane restarts with a size write but no address write.

The MAME trace replay checks whether real OS activity ever hits either case.
