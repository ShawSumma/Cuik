// special
R(sll,   0b000000, 0b000000, MIPSImm)
R(srl,   0b000000, 0b000010, MIPSImm)
R(dsll,  0b000000, 0b111000, MIPSImm)
R(dsrl,  0b000000, 0b111010, MIPSImm)
R(sllv,  0b000000, 0b000100, void)
R(srlv,  0b000000, 0b000110, void)
R(srav,  0b000000, 0b000111, void)
R(sra,   0b000000, 0b000011, void)
R(add,   0b000000, 0b100000, void)
R(addu,  0b000000, 0b100001, void)
R(dadd,  0b000000, 0b101100, void)
R(daddu, 0b000000, 0b101101, void)
R(sub,   0b000000, 0b100010, void)
R(subu,  0b000000, 0b100011, void)
R(dsub,  0b000000, 0b101110, void)
R(dsubu, 0b000000, 0b101111, void)
R(and,   0b000000, 0b100100, void)
R(or,    0b000000, 0b100101, void)
R(xor,   0b000000, 0b100110, void)
R(nor,   0b000000, 0b100111, void)
R(jr,    0b000000, 0b001000, void)
// special2
R(mul,   0b011100, 0b000010, void)
// i-types
I(addi,   0b001000,          MIPSImm)
I(daddi,  0b011000,          MIPSImm)
I(andi,   0b001100,          MIPSImm)
I(addiu,  0b001001,          MIPSImm)
I(daddiu, 0b011001,          MIPSImm)
I(ori,    0b001101,          MIPSImm)
I(xori,   0b001110,          MIPSImm)
I(lui,    0b001111,          MIPSImm)
// loads
I(lb,    0b100000,           MIPSImm)
I(lw,    0b100011,           MIPSImm)
I(ld,    0b110111,           MIPSImm)
// stores
I(sb,    0b101000,           MIPSImm)
I(sh,    0b101001,           MIPSImm)
I(sw,    0b101011,           MIPSImm)
// floats
I(ldc1,  0b110101,           MIPSImm)
I(sdc1,  0b111101,           MIPSImm)
R(madds, 0b010011, 0b100000, void)
R(maddd, 0b010011, 0b100001, void)
// branches
I(beq,   0b000100,           void)
J(jal,   0b000011,           void)
#undef R
#undef I
#undef J