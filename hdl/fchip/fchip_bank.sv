// The EIII's 16 digital voice lanes and the sample fetch for one output frame.
// Writes, snapshot loads and reads address a lane by voice number; their meaning and the ticks are as in fchip_lane.
//
// Sample fetch: on frame_req every lane's current output is latched (address and sounding flag at that cycle), then
// read from sample memory one lane at a time over a simple request / valid port (one outstanding read), and
// frame_done pulses with all 16 values in pcm.  A lane that is not sounding, or whose address is past the memory
// (mem_words), outputs 0, as eiii::fchip_bank::pcm().
module fchip_bank #(
	parameter int VOICES = 16
) (
	input  logic        clk,
	input  logic        rst,
	input  logic        tick,
	input  logic        rf_we,
	input  logic [3:0]  rf_voice,
	input  logic [1:0]  rf_slot,
	input  logic [24:0] rf_wdata,
	input  logic        ctrl_we,
	input  logic [3:0]  ctrl_voice,
	input  logic [7:0]  ctrl_wdata,
	input  logic        ld_we,
	input  logic [3:0]  ld_voice,
	input  logic [11:0] ld_phase,
	input  logic [11:0] ld_rate,
	input  logic        ld_active,
	input  logic        ld_ctrl_phase,
	input  logic [3:0]  ld_ctrl_hi,
	input  logic [3:0]  rd_voice,
	input  logic [1:0]  rd_slot,
	output logic [24:0] rd_data,
	// sample memory, 16-bit words
	input  logic [22:0] mem_words,
	output logic        mem_req,
	output logic [21:0] mem_addr,
	input  logic        mem_valid,
	input  logic [15:0] mem_rdata,
	// output frame
	input  logic        frame_req,
	output logic        frame_done,
	output logic signed [15:0] pcm [VOICES]
);
	logic [24:0] rd [VOICES];
	logic [21:0] addr [VOICES];
	logic        sounding [VOICES];

	for (genvar v = 0; v < VOICES; ++v)
	begin : lane
		fchip_lane u (
			.clk, .rst, .tick,
			.rf_we(rf_we && rf_voice == 4'(v)), .rf_slot, .rf_wdata,
			.ctrl_we(ctrl_we && ctrl_voice == 4'(v)), .ctrl_wdata,
			.ld_we(ld_we && ld_voice == 4'(v)), .ld_phase, .ld_rate, .ld_active, .ld_ctrl_phase, .ld_ctrl_hi,
			.rd_slot, .rd_data(rd[v]),
			.addr(addr[v]), .sounding(sounding[v])
		);
	end
	assign rd_data = rd[rd_voice];

	// fetch sequencer
	logic [21:0] snap_addr [VOICES];
	logic        snap_on [VOICES];
	logic        busy, waiting;
	logic [4:0]  idx;

	always_ff @(posedge clk)
		if (rst)
		begin
			busy <= 1'b0; waiting <= 1'b0; idx <= '0; mem_req <= 1'b0; frame_done <= 1'b0;
			for (int v = 0; v < VOICES; ++v) pcm[v] <= '0;
		end
		else
		begin
			frame_done <= 1'b0;
			mem_req <= 1'b0;
			if (frame_req && !busy)
			begin
				for (int v = 0; v < VOICES; ++v) begin snap_addr[v] <= addr[v]; snap_on[v] <= sounding[v]; end
				busy <= 1'b1; idx <= '0; waiting <= 1'b0;
			end
			else if (busy && !waiting)
			begin
				if (idx == 5'(VOICES))
				begin
					busy <= 1'b0; frame_done <= 1'b1;
				end
				else if (snap_on[idx[3:0]] && {1'b0, snap_addr[idx[3:0]]} < mem_words)
				begin
					mem_req <= 1'b1; mem_addr <= snap_addr[idx[3:0]]; waiting <= 1'b1;
				end
				else
				begin
					pcm[idx[3:0]] <= '0; idx <= idx + 5'd1;
				end
			end
			else if (waiting && mem_valid)
			begin
				pcm[idx[3:0]] <= mem_rdata; idx <= idx + 5'd1; waiting <= 1'b0;
			end
		end
endmodule
