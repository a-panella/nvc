--
-- Grab bag of miscellaneous VHDL-2008 syntax
--

entity vhdl2008 is
end entity;

package genpack is
    generic ( x : integer := 5; y : boolean );  -- OK
    generic map ( x => 5, y => false );         -- OK

    constant c : bit_vector(1 to x) := (1 to x => '1');
end package;

package genpack2 is
    generic ( x : integer := 5; y : boolean );  -- OK
    function add_x_if_y ( arg : integer ) return integer;
end package;

package body genpack2 is
    function add_x_if_y ( arg : integer ) return integer is
    begin
        if y then
            return arg + x;
        else
            return arg;
        end if;
    end function;
end package body;

package primary_genpack2 is new work.genpack2 generic map (4, false);  -- OK

architecture test of vhdl2008 is
    type my_utype is (a, b, c);
    type my_utype_vector is array (natural range <>) of my_utype;

    function resolved (s : my_utype_vector) return my_utype;

    subtype my_type is resolved my_utype;
    subtype my_type_vector is (resolved) my_utype_vector;  -- OK

    type my_logical_vec is array (natural range <>) of bit;

    type my_bool is (true, false);
    package my_genpack2 is new work.genpack2 generic map (1, true);  -- OK
begin

    process is
        variable b : bit;
        variable v : my_logical_vec(1 to 3);
    begin
        b := or v;                      -- OK
        if or v = '1' then end if;      -- OK
        b := and v;                     -- OK
        b := xor v;                     -- OK
        b := xnor v;                    -- OK
        b := nand v;                    -- OK
        b := nor v;                     -- OK
    end process;

    process is
        variable b : bit;
        variable v : my_logical_vec(1 to 3);
    begin
        b := b ?= '1';                  -- OK
        b := b ?/= '1';                 -- OK
        b := b ?< '0';                  -- OK
        b := b ?> '0';                  -- OK
        b := b ?<= '1';                 -- OK
        b := b ?>= '1';                 -- OK
        b := v ?= "101";                -- OK
        b := v ?/= "111";               -- OK
    end process;

    process is
        variable b : bit;
        variable i : integer;
        function "??"(x : integer) return boolean;
    begin
        if b then end if;               -- OK
        if b xor '1' then end if;       -- OK
        while b and '1' loop end loop;  -- OK
        if i + 1 then end if;           -- OK
        if now + 1 ns then end if;      -- Error
        while true loop
            exit when b or '1';         -- OK
            next when b or '1';         -- OK
        end loop;
        wait until b xor '0';           -- OK
        assert b nor '1';               -- OK
        assert ?? 1;                    -- OK
    end process;

    /* This is a comment */

    /* Comments /* do not nest */

    process is
        variable x, y : integer;
    begin
        x := 1 when y > 2 else 5;       -- OK
    end process;

    process is
        variable x : string(7 downto 0);
    begin
        x := 8x"0";                     -- OK
        x := 6x"a";                     -- OK
        x := 4x"4";                     -- OK
        x := 2x"4";                     -- Error
        x := 0x"5";                     -- Error
        x := 18x"383fe";                -- OK
        x := 0b"0000";                  -- OK
        x := d"5";                      -- OK
        x := 5d"25";                    -- OK
        x := 120d"83298148949012041209428481024019511";  -- Error
        x := uo"5";                     -- OK
        x := 5sb"11";                   -- OK
        x := 2sb"1111110";              -- OK
        x := 2sb"10110101";             -- Error
        x := 4x"0f";                    -- OK
        x := Uo"2C";                    -- OK
        x := d"C4";                     -- Error
        x := 8x"-";                     -- OK
        x := 12d"13";                   -- OK
    end process;

    b2: block is
        signal s : integer;
    begin

        process is
        begin
            s <= 1 when s < 0 else 5;   -- OK
        end process;

    end block;

    process is
        type int_vec2 is array (natural range <>) of integer_vector;  -- OK
        constant a : int_vec2(1 to 3)(1 to 2) := (  -- OK
            (1, 2), (3, 4), (5, 6) );
    begin
        assert a(1)(1) = 1;             -- OK
    end process;

    b3: block is
        signal s : integer;
    begin
        process is
        begin
            s <= force 1;               -- OK
            s <= force out 1;           -- OK
            s <= force in 2;            -- OK
            s <= release;               -- OK
            s <= release out;           -- OK
        end process;
    end block;

    process is
        variable x : bit_vector(1 to 3);
    begin
        case? x is                      -- OK
            when "010" => null;
            when others => null;
        end case?;
        case? x is
            when others => null;
        end case;                       -- Error
        case x is
            when others => null;
        end case ?;                     -- Error
    end process;

    b4: block is
        procedure foo (x : integer_vector) is
            variable a : x'subtype;     -- OK
            variable b : integer'subtype;  -- OK
            variable c : b4'subtype;    -- Error
        begin
        end procedure;
    begin
    end block;

end architecture;
