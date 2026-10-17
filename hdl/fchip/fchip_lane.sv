// One EIII digital voice lane: F-Chip rate divider + microcontroller register file.
// Cycle-level port of fchip/eiii_fchip.cpp (the bit-exact C++ model, checked against MAME by tests/fchip_equiv).
// Semantics: analysis/DIGITAL_VOICE_HARDWARE.md.
//
// tick: one cycle of the 20 MHz F-Chip clock (C20M).  The C++ model's advance(n) equals n ticks, and an event at
// time t lands after the ticks up to t, so the host applies a write in a cycle with tick low.
//
// Known difference: advance() can take several steps at once when a rate write leaves the divide count at or past
// twice the new divisor (a downward pitch jump of more than an octave mid-count).  Here a lane takes at most one step
// per tick and keeps the remainder, so both agree whenever count + 1 < 2 * N.
// Second difference: when a one-shot ends partway through an advance(), the C++ model leaves the divide count at that
// whole call's remainder, so its value depends on how the caller chunked time.  Here the count stops at 0 when the lane
// ends.  It only shows if a lane is restarted by a current-size write without a new current address (which resets
// the count).
module fchip_lane (
	input  logic        clk,
	input  logic        rst,
	input  logic        tick,
	// register file write: a complete word (MS word committed), slot 0..3 = +0 / +4 / +8 / +C
	input  logic        rf_we,
	input  logic [1:0]  rf_slot,
	input  logic [24:0] rf_wdata,
	// F-Chip control port byte (0xD30010 + 2 * voice): high byte then low byte, divisor = hi[3:0]:lo
	input  logic        ctrl_we,
	input  logic [7:0]  ctrl_wdata,
	// snapshot restore (a saved project, or a trace's start state): divide count, divisor, active flag and control-port
	// byte position; the register-file words are restored with rf writes before this
	input  logic        ld_we,
	input  logic [11:0] ld_phase,
	input  logic [11:0] ld_rate,
	input  logic        ld_active,
	input  logic        ld_ctrl_phase,
	input  logic [3:0]  ld_ctrl_hi,
	input  logic [1:0]  rd_slot,
	output logic [24:0] rd_data,
	output logic [21:0] addr,      // current sample address (RFD00-21)
	output logic        sounding   // active and rate set: the lane outputs ram[addr], else 0
);
	localparam logic [21:0] DATA = 22'h3fffff;
	localparam int FORBK = 22, DEC = 23, LOOP = 24;

	logic [24:0] size, init_size, cur, init_addr;
	logic [11:0] rate, phase;
	logic        active, ctrl_phase;
	logic [3:0]  ctrl_hi;   // only the divisor's top bits are kept

	function automatic logic [24:0] step(input logic [24:0] w);
		step = {w[24:22], w[DEC] ? w[21:0] - 22'd1 : w[21:0] + 22'd1};
	endfunction

	wire         at_end  = size[DEC] ? size[21:0] == 22'd0 : size[21:0] == DATA;
	wire  [12:0] next_ph = {1'b0, phase} + 13'd1;
	wire         go      = tick && active && rate != 12'd0;
	wire         stepnow = next_ph >= {1'b0, rate};

	always_ff @(posedge clk)
		if (rst)
		begin
			size <= '0; init_size <= '0; cur <= '0; init_addr <= '0;
			rate <= '0; phase <= '0; active <= 1'b0; ctrl_phase <= 1'b0; ctrl_hi <= '0;
		end
		else
		begin
			if (go)
			begin
				phase <= stepnow ? 12'(next_ph - {1'b0, rate}) : next_ph[11:0];
				if (stepnow)
				begin
					if (!at_end)
					begin
						size <= step(size);
						cur  <= step(cur);
					end
					else if (size[FORBK])
					begin
						// forward-backward turnaround: one more step, flip direction, reload size
						cur  <= step(cur) ^ (25'd1 << DEC);
						size <= init_size;
					end
					else if (size[LOOP])
					begin
						size <= init_size;
						cur  <= init_addr;
					end
					else
						active <= 1'b0;   // one-shot end: silence
				end
			end
			if (rf_we)
				case (rf_slot)
				2'd0: begin size <= rf_wdata; active <= 1'b1; end   // new remaining count
				2'd1: init_size <= rf_wdata;
				2'd2: begin cur <= rf_wdata; phase <= '0; end      // new address restarts the divide-by-N count
				2'd3: init_addr <= rf_wdata;
				endcase
			if (ctrl_we)
			begin
				ctrl_phase <= !ctrl_phase;
				if (!ctrl_phase) ctrl_hi <= ctrl_wdata[3:0];
				else rate <= {ctrl_hi, ctrl_wdata};
			end
			if (ld_we)
			begin
				phase <= ld_phase; rate <= ld_rate; active <= ld_active; ctrl_phase <= ld_ctrl_phase; ctrl_hi <= ld_ctrl_hi;
			end
		end

	always_comb
		case (rd_slot)
		2'd0: rd_data = size;
		2'd1: rd_data = init_size;
		2'd2: rd_data = cur;
		default: rd_data = init_addr;
		endcase

	assign addr = cur[21:0];
	assign sounding = active && rate != 12'd0;
endmodule
