interface p3_if(input logic clk);
  logic        valid;
  logic        ready;
  logic [7:0]  payload;

  modport master(output valid, output payload, input ready);
  modport slave(input valid, input payload, output ready);
endinterface

module p3_leaf #(
  parameter int WIDTH = 8,
  parameter logic [1:0] RESET_STATE = 2'd0
) (
  input  logic             clk,
  input  logic             rst_n,
  input  logic             inc,
  input  logic             dec,
  input  logic             hold,
  input  logic             sel,
  input  logic [WIDTH-1:0] a,
  input  logic [WIDTH-1:0] b,
  output logic [WIDTH-1:0] out,
  output logic [WIDTH-1:0] count,
  output logic [1:0]       state,
  p3_if.slave              bus
);
  typedef enum logic [1:0] {
    ST_IDLE = 2'd0,
    ST_RUN  = 2'd1,
    ST_DONE = 2'd2
  } state_e;

  state_e state_q;
  state_e state_d;
  logic [WIDTH-1:0] packed_arr [0:1];
  logic [WIDTH-1:0] copy_value;

  assign bus.ready = (state_q == ST_RUN) && !hold;
  assign state = state_q;
  assign copy_value = a;

  always_comb begin
    packed_arr[0] = a;
    packed_arr[1] = b;
    out = '0;
    unique case (state_q)
      ST_IDLE: out = {a[3:0], b[3:0]};
      ST_RUN: begin
        if (sel) begin
          out = packed_arr[0] + packed_arr[1];
        end else begin
          out = copy_value[WIDTH-1:0];
        end
      end
      default: out = b;
    endcase
  end

  always_comb begin
    state_d = state_q;
    unique case (state_q)
      ST_IDLE: if (bus.valid) state_d = ST_RUN;
      ST_RUN:  if (hold)      state_d = ST_DONE;
      default:                state_d = ST_IDLE;
    endcase
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      count <= '0;
      state_q <= state_e'(RESET_STATE);
    end else begin
      state_q <= state_d;
      if (inc && !dec) begin
        count <= count + 1'b1;
      end else if (!inc && dec) begin
        count <= count - 1'b1;
      end else begin
        count <= count;
      end
    end
  end
endmodule

module p3_mid(
  input  logic       clk,
  input  logic       rst_n,
  input  logic       inc,
  input  logic       dec,
  input  logic       hold,
  input  logic       sel,
  input  logic [7:0] a,
  input  logic [7:0] b,
  output logic [7:0] out,
  output logic [7:0] count,
  output logic [1:0] state,
  p3_if.slave        bus_in
);
  p3_leaf u_leaf (
    .clk(clk),
    .rst_n(rst_n),
    .inc(inc),
    .dec(dec),
    .hold(hold),
    .sel(sel),
    .a(a),
    .b(b),
    .out(out),
    .count(count),
    .state(state),
    .bus(bus_in)
  );
endmodule

module p3_sem_top;
  logic clk = 1'b0;
  logic rst_n = 1'b0;
  logic inc = 1'b0;
  logic dec = 1'b0;
  logic hold = 1'b0;
  logic sel = 1'b0;
  logic [7:0] a = 8'h12;
  logic [7:0] b = 8'h34;
  logic [7:0] out;
  logic [7:0] count;
  logic [1:0] state;

  p3_if bus(clk);

  p3_mid u_mid (
    .clk(clk),
    .rst_n(rst_n),
    .inc(inc),
    .dec(dec),
    .hold(hold),
    .sel(sel),
    .a(a),
    .b(b),
    .out(out),
    .count(count),
    .state(state),
    .bus_in(bus)
  );
endmodule
