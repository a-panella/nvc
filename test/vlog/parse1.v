module parse1;
  wire [8:0] x, y, z;
  reg        z;
  assign y = x;
  always begin : foo
    $display("hello");
    $finish;
    if (x);
    if (x) z <= 1;
    if (x) $display("yes");
    else if (z);
    else $display("no");
    z = 0;
  end
  assign x = x | y;
endmodule // parse1
