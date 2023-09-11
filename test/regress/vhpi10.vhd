entity vhpi10 is
    generic (
        g0 : integer := 42;
        g1 : string := "hello" );
end entity;

architecture test of vhpi10 is
    constant c0 : integer := 5;
    constant c1 : real := 1.5;
begin

    b0: block is
        signal s0 : bit;
    begin
    end block;

    b1: block (true) is
        type t_abc is (a, b, c);
        signal s0 : t_abc;
    begin
    end block;

end architecture;