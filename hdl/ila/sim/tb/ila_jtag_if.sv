// SPDX-License-Identifier: Apache-2.0
// JTAG interface for OwlTAP ILA UVM environment
`timescale 1ns/1ps

interface ila_jtag_if (input logic tck);
    logic tms;
    logic tdi;
    wire  tdo;     // driven by DUT (or testbench assign) — must be wire for continuous assignment
    logic trst_n;

    // Driver modport
    modport drv (
        input  tck,
        output tms, tdi, trst_n,
        input  tdo
    );

    // Monitor modport
    modport mon (
        input tck, tms, tdi, tdo, trst_n
    );
endinterface
