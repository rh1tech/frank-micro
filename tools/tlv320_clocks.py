#!/usr/bin/env python3
"""Work out TLV320DAC3100 clock settings for frank-micro, and check them.

The Fruit Jam's audio codec is silent until its clock tree is configured over
I2C.  Getting those numbers wrong is the kind of mistake that produces sound at
the wrong pitch, or no sound at all, with nothing on screen to explain it.  So
the arithmetic lives here rather than as constants in a C file, with every
constraint written next to the datasheet clause it comes from.

Run it with no arguments for the frank-micro answer plus an audit of the two
Adafruit projects we borrowed the register sequence from:

    python3 tools/tlv320_clocks.py

Run the self-test after changing anything:

    python3 tools/tlv320_clocks.py --self-test

Sources.  All clause references are to the TLV320DAC3100 datasheet, TI document
SLAS671C, February 2010, revised January 2017.  Register numbers are page 0
unless stated.  The bit clock ratio comes from this repository:
drivers/audio_i2s.pio is the 16-bit stereo I2S program at two PIO cycles per
bit, and drivers/audio.c:121-123 sets the divider to sys_clk * 4 / sample_freq
used as 8.8 fixed point, which is 64 PIO cycles and so 32 bit clocks per stereo
frame.  pico-extras uses the same program and the same formula, so the Adafruit
projects have the same ratio.

Nothing here has been tested on hardware.  The arithmetic is checkable; whether
the codec locks is not, until there is a board.
"""

from fractions import Fraction
import argparse
import itertools
import sys

# ---------------------------------------------------------------------------
# Datasheet constraints
# ---------------------------------------------------------------------------

# PLL input range, section 6.3.11.1: "The PLL input supports clocks varying
# from 512 kHz to 20 MHz".
PLL_IN_MIN = 512_000
PLL_IN_MAX = 20_000_000

# PLL output range, equations 7 and 8: "80 MHz <= (PLL_CLKIN x J.D x R / P)
# <= 110 MHz".  Equation 7 adds "4 <= R x J <= 259" when D = 0, and equation 8
# requires R = 1 when D is non-zero.  We only ever want D = 0, because a
# fractional multiplier cannot give an exact ratio.
PLL_OUT_MIN = 80_000_000
PLL_OUT_MAX = 110_000_000
RJ_MIN, RJ_MAX = 4, 259

# Divider ranges.  R and J and P from section 6.3.11.1, NDAC and MDAC and DOSR
# from the clock tree figure and section 6.3.10.14.
R_VALUES = range(1, 17)          # 1 to 16,  register 5 bits D3-D0
J_VALUES = range(1, 64)          # 1 to 63,  register 6 bits D5-D0
P_VALUES = range(1, 9)           # 1 to 8,   register 5 bits D6-D4
NDAC_MAX = 128                   # register 11 bits D6-D0
MDAC_MAX = 128                   # register 12 bits D6-D0
DOSR_MAX = 1024                  # registers 13 and 14

# Maximum clock frequencies, table 6-27, for DVDD >= 1.65 V.  The Fruit Jam
# runs the codec's DVDD from a dedicated 1.8 V regulator, so this column is the
# applicable one.
CODEC_CLKIN_MAX = 110_000_000
DAC_CLK_MAX = 49_152_000
DAC_MOD_CLK_MAX_TABLE = 6_758_000
DAC_FS_MAX = 192_000

# Section 6.3.10.14 step 1: "2.8 MHz < DOSR x DAC_fS < 6.2 MHz".  This is
# tighter than the table 6-27 figure above and is the one that binds.  Note it
# is a strict inequality in the datasheet, so it is treated as one here.
DAC_MOD_CLK_MIN = 2_800_000
DAC_MOD_CLK_MAX = 6_200_000

# Section 6.3.10.14 step 1: DOSR must be an integral multiple of the
# interpolation ratio, which is 8 for filter A, 4 for filter B and 2 for
# filter C.
INTERPOLATION = {"A": 8, "B": 4, "C": 2}

# Resource class per processing block, table 6-11.  Section 6.3.10.14 requires
# MDAC x DOSR / 32 >= RC, and advises making NDAC as large as the constraint
# allows.  Register 60 selects the block and resets to 0 0001, PRB_P1, so RC 8
# and filter A are the defaults a driver gets if it writes nothing.
RESOURCE_CLASS = {
    "PRB_P1": (8, "A"), "PRB_P2": (12, "A"), "PRB_P3": (10, "A"),
    "PRB_P4": (4, "A"), "PRB_P5": (6, "A"), "PRB_P6": (6, "A"),
    "PRB_P7": (6, "B"), "PRB_P8": (8, "B"), "PRB_P9": (8, "B"),
    "PRB_P10": (10, "B"), "PRB_P11": (8, "B"), "PRB_P12": (3, "B"),
    "PRB_P13": (4, "B"), "PRB_P14": (4, "B"), "PRB_P15": (6, "B"),
    "PRB_P16": (4, "B"), "PRB_P17": (3, "C"), "PRB_P18": (6, "C"),
    "PRB_P19": (4, "C"), "PRB_P20": (2, "C"), "PRB_P21": (3, "C"),
    "PRB_P22": (2, "C"), "PRB_P23": (8, "A"), "PRB_P24": (12, "A"),
    "PRB_P25": (12, "A"),
}
DEFAULT_BLOCK = "PRB_P1"

# Reset values, from the register tables in section 6.6.  A driver that does
# not write these gets them, which is how both Adafruit projects end up at
# DOSR 128 without ever writing registers 13 and 14.
RESET = {"P": 1, "R": 1, "J": 4, "D": 0, "NDAC": 1, "MDAC": 1, "DOSR": 128}

# This repository's I2S bit clock is 32 times the sample rate.  See the module
# docstring for why.
BCLK_RATIO = 32


# ---------------------------------------------------------------------------
# The clock tree
# ---------------------------------------------------------------------------

class Config:
    """One complete set of codec clock settings, plus what it computes to."""

    def __init__(self, fs, bclk_ratio=BCLK_RATIO, block=DEFAULT_BLOCK,
                 P=1, R=1, J=4, NDAC=1, MDAC=1, DOSR=128):
        self.fs = fs
        self.bclk_ratio = bclk_ratio
        self.block = block
        self.P, self.R, self.J = P, R, J
        self.NDAC, self.MDAC, self.DOSR = NDAC, MDAC, DOSR

    # PLL_CLKIN is BCLK, because register 4 bits D3-D2 select BCLK as the PLL
    # input.  That is what lets the codec run with the MCLK pin undriven.
    @property
    def pll_clkin(self):
        return Fraction(self.fs * self.bclk_ratio)

    # Equation 6: PLL_CLK = PLL_CLKIN x R x J.D / P, with D = 0 throughout.
    @property
    def pll_clk(self):
        return self.pll_clkin * self.R * self.J / self.P

    # Register 4 bits D1-D0 select PLL_CLK as CODEC_CLKIN.
    @property
    def codec_clkin(self):
        return self.pll_clk

    @property
    def dac_clk(self):
        return self.codec_clkin / self.NDAC

    @property
    def dac_mod_clk(self):
        return self.dac_clk / self.MDAC

    # Section 6.3.10.14: CODEC_CLKIN = NDAC x MDAC x DOSR x DAC_fS.  This is
    # the rate the codec actually plays at, which equals self.fs only when the
    # divider chain is exact.
    @property
    def dac_fs(self):
        return self.dac_mod_clk / self.DOSR

    @property
    def resource_class(self):
        return RESOURCE_CLASS[self.block][0]

    @property
    def filter_type(self):
        return RESOURCE_CLASS[self.block][1]

    def checks(self):
        """Every datasheet constraint, as (ok, name, detail) triples."""
        rc = self.resource_class
        interp = INTERPOLATION[self.filter_type]
        out = [
            (PLL_IN_MIN <= self.pll_clkin <= PLL_IN_MAX,
             "PLL input in 0.512 to 20 MHz",
             f"PLL_CLKIN = {mhz(self.pll_clkin)}"),
            (PLL_OUT_MIN <= self.pll_clk <= PLL_OUT_MAX,
             "PLL output in 80 to 110 MHz",
             f"PLL_CLK = {mhz(self.pll_clk)}"),
            (RJ_MIN <= self.R * self.J <= RJ_MAX,
             "4 <= R x J <= 259 with D = 0",
             f"R x J = {self.R * self.J}"),
            (self.codec_clkin <= CODEC_CLKIN_MAX,
             "CODEC_CLKIN <= 110 MHz",
             f"CODEC_CLKIN = {mhz(self.codec_clkin)}"),
            (self.dac_clk <= DAC_CLK_MAX,
             "DAC_CLK <= 49.152 MHz",
             f"DAC_CLK = {mhz(self.dac_clk)}"),
            (DAC_MOD_CLK_MIN < self.dac_mod_clk < DAC_MOD_CLK_MAX,
             "2.8 MHz < DAC_MOD_CLK < 6.2 MHz",
             f"DAC_MOD_CLK = {mhz(self.dac_mod_clk)}"),
            (self.dac_mod_clk <= DAC_MOD_CLK_MAX_TABLE,
             "DAC_MOD_CLK <= 6.758 MHz, table 6-27",
             f"DAC_MOD_CLK = {mhz(self.dac_mod_clk)}"),
            (self.dac_fs <= DAC_FS_MAX,
             "DAC_fS <= 192 kHz",
             f"DAC_fS = {khz(self.dac_fs)}"),
            (Fraction(self.MDAC * self.DOSR, 32) >= rc,
             f"MDAC x DOSR / 32 >= RC, RC = {rc} for {self.block}",
             f"MDAC x DOSR / 32 = {Fraction(self.MDAC * self.DOSR, 32)}"),
            (self.DOSR % interp == 0,
             f"DOSR a multiple of {interp}, filter {self.filter_type}",
             f"DOSR = {self.DOSR}"),
            (1 <= self.NDAC <= NDAC_MAX and 1 <= self.MDAC <= MDAC_MAX
             and 1 <= self.DOSR <= DOSR_MAX,
             "NDAC, MDAC in 1 to 128 and DOSR in 1 to 1024",
             f"NDAC = {self.NDAC}, MDAC = {self.MDAC}, DOSR = {self.DOSR}"),
            (self.dac_fs == self.fs,
             "divider chain exact, DAC_fS equals the requested rate",
             f"DAC_fS = {khz(self.dac_fs)} against {khz(self.fs)} requested"),
        ]
        return out

    def ok(self):
        return all(c[0] for c in self.checks())

    def registers(self):
        """The page 0 register writes this configuration needs, in order.

        Encoding quirks, all from the register tables in section 6.6: in the P
        field 000 means 8; in the R field 0000 means 16; in NDAC and MDAC
        000 0000 means 128; and in the DOSR LSB, 0 with an MSB of 00 means
        1024.  So a value of N is written as N except at the top of its range.
        """
        p_field = 0 if self.P == 8 else self.P
        r_field = 0 if self.R == 16 else self.R
        ndac_field = 0 if self.NDAC == 128 else self.NDAC
        mdac_field = 0 if self.MDAC == 128 else self.MDAC
        dosr = 0 if self.DOSR == 1024 else self.DOSR
        return [
            (0x04, 0b0000_01_11,
             "PLL_CLKIN = BCLK, CODEC_CLKIN = PLL_CLK"),
            (0x06, self.J,
             f"PLL J = {self.J}"),
            (0x07, 0x00, "PLL D MSB = 0"),
            (0x08, 0x00, "PLL D LSB = 0, so R may be above 1"),
            (0x05, (p_field << 4) | r_field,
             f"PLL P = {self.P}, R = {self.R}, still powered down"),
            (0x05, 0x80 | (p_field << 4) | r_field,
             f"same, PLL powered up. Wait 10 ms for PLL_CLK"),
            (0x0B, 0x80 | ndac_field,
             f"NDAC = {self.NDAC}, powered up"),
            (0x0C, 0x80 | mdac_field,
             f"MDAC = {self.MDAC}, powered up"),
            (0x0D, (dosr >> 8) & 0x03,
             f"DOSR MSB, DOSR = {self.DOSR}"),
            (0x0E, dosr & 0xFF,
             "DOSR LSB, must follow register 13 immediately"),
        ]

    def __str__(self):
        return (f"fs {khz(self.fs)}  BCLK {khz(self.pll_clkin)}  "
                f"P {self.P} R {self.R} J {self.J}  "
                f"NDAC {self.NDAC} MDAC {self.MDAC} DOSR {self.DOSR}  "
                f"PLL {mhz(self.pll_clk)}  DAC_MOD {mhz(self.dac_mod_clk)}")


def mhz(x):
    return f"{float(x) / 1e6:.6f} MHz".replace(".000000 ", " ")


def khz(x):
    return f"{float(x) / 1e3:.4f} kHz".replace(".0000 ", " ")


# ---------------------------------------------------------------------------
# Solving
# ---------------------------------------------------------------------------

def solve(fs, bclk_ratio=BCLK_RATIO, block=DEFAULT_BLOCK, limit=None):
    """Every valid configuration for this sample rate, best first.

    The search is exhaustive over P, R, J and DOSR.  Once those are fixed the
    product NDAC x MDAC is determined, because the divider chain has to be
    exact, so only its factorisations need trying.  Ranking follows the
    datasheet's advice in section 6.3.10.14: make NDAC as large as the
    MDAC x DOSR constraint allows.  Ties break towards a smaller PLL output,
    which is the lower-power choice.
    """
    rc, filt = RESOURCE_CLASS[block]
    interp = INTERPOLATION[filt]
    bclk = fs * bclk_ratio
    found = []

    if not PLL_IN_MIN <= bclk <= PLL_IN_MAX:
        return found

    dosr_lo = max(interp, (DAC_MOD_CLK_MIN // fs) + 1)
    dosr_hi = min(DOSR_MAX, (DAC_MOD_CLK_MAX - 1) // fs)

    for P, R, J in itertools.product(P_VALUES, R_VALUES, J_VALUES):
        if not RJ_MIN <= R * J <= RJ_MAX:
            continue
        pll = Fraction(bclk * R * J, P)
        if not PLL_OUT_MIN <= pll <= PLL_OUT_MAX:
            continue
        for DOSR in range(dosr_lo, dosr_hi + 1):
            if DOSR % interp:
                continue
            # CODEC_CLKIN = NDAC x MDAC x DOSR x fs must hold exactly.
            product = pll / (DOSR * fs)
            if product.denominator != 1:
                continue
            product = int(product)
            for MDAC in range(1, MDAC_MAX + 1):
                if product % MDAC:
                    continue
                NDAC = product // MDAC
                cfg = Config(fs, bclk_ratio, block, P, R, J, NDAC, MDAC, DOSR)
                if cfg.ok():
                    found.append(cfg)

    found.sort(key=lambda c: (-c.NDAC, c.pll_clk, c.P, c.R))
    return found[:limit] if limit else found


def common(rates, bclk_ratio=BCLK_RATIO, block=DEFAULT_BLOCK):
    """Configurations whose register values are valid at every listed rate.

    Worth asking because the PLL runs from the bit clock, so the whole chain
    scales with the sample rate and one register set can cover several.  That
    is also how both Adafruit projects came to share a register block; see the
    audit below for what it costs them.
    """
    keyed = None
    for fs in rates:
        here = {(c.P, c.R, c.J, c.NDAC, c.MDAC, c.DOSR) for c in solve(
            fs, bclk_ratio, block)}
        keyed = here if keyed is None else (keyed & here)
    out = [Config(rates[0], bclk_ratio, block, *k) for k in sorted(
        keyed or [], key=lambda k: (-k[3], k[0], k[1]))]
    return out


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def report(cfg, title):
    print(f"\n{title}")
    print(f"  {cfg}")
    for ok, name, detail in cfg.checks():
        print(f"    {'pass' if ok else 'FAIL'}  {name:52s} {detail}")


def print_registers(cfg):
    print("\n  Register writes, page 0:")
    for reg, val, why in cfg.registers():
        print(f"    0x{reg:02X} = 0x{val:02X}   {why}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--rate", type=int, action="append",
                    help="sample rate in Hz, repeatable. "
                         "Default 31250 and 32000")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    rates = args.rate or [31250, 32000]

    print("=" * 78)
    print("frank-micro on the Adafruit Fruit Jam")
    print("=" * 78)
    print(f"\nI2S bit clock is {BCLK_RATIO} x the sample rate, and the codec "
          f"PLL runs from it,\nso the MCLK pin need not be driven. Processing "
          f"block {DEFAULT_BLOCK} is the reset\ndefault: resource class "
          f"{RESOURCE_CLASS[DEFAULT_BLOCK][0]}, interpolation filter "
          f"{RESOURCE_CLASS[DEFAULT_BLOCK][1]}.")

    for fs in rates:
        best = solve(fs, limit=3)
        print(f"\n{len(solve(fs))} valid configurations at {khz(fs)}. "
              f"Best three by the datasheet's own ranking:")
        for cfg in best:
            print(f"  {cfg}")

    shared = common(rates)
    print(f"\n{len(shared)} configurations are valid at every rate in "
          f"{[khz(r) for r in rates]}.")
    if shared:
        pick = shared[0]
        report(Config(rates[0], BCLK_RATIO, DEFAULT_BLOCK, pick.P, pick.R,
                      pick.J, pick.NDAC, pick.MDAC, pick.DOSR),
               f"Recommended, checked at {khz(rates[0])}")
        for fs in rates[1:]:
            report(Config(fs, BCLK_RATIO, DEFAULT_BLOCK, pick.P, pick.R,
                          pick.J, pick.NDAC, pick.MDAC, pick.DOSR),
                   f"The same register values at {khz(fs)}")
        print_registers(pick)

    print("\n" + "=" * 78)
    print("Audit of the two Adafruit projects we borrowed the sequence from")
    print("=" * 78)
    print("\nBoth write P = 1, R = 2, J = 32, NDAC = 8, MDAC = 2 and leave "
          "DOSR at its\nreset value of 128. Neither writes registers 13 or 14. "
          "The reason one register\nblock appears in both is that "
          "NDAC x MDAC x DOSR = 2048 = 32 x R x J / P, an\nidentity in the "
          "ratios that holds at any sample rate. What does not hold at any\n"
          "sample rate is the absolute limits.")

    borrowed = dict(P=1, R=2, J=32, NDAC=8, MDAC=2, DOSR=128)
    report(Config(22256, **borrowed),
           "pico-mac, 22256 Hz (src/main.c, sample_freq and the register block)")
    report(Config(49716, **borrowed),
           "fruitjam-doom, 49716 Hz (src/pico/i_picosound.h:27)")
    report(Config(31250, **borrowed),
           "the same values at frank-micro's native 31250 Hz")

    print("\nSo the borrowed register block must not be copied. The sequence "
          "around it,\nthe reset, the routing and the mute handling, is still "
          "worth taking.")
    return 0


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

def self_test():
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)

    # Register encoding quirks round-trip.
    c = Config(31250, P=8, R=16, NDAC=128, MDAC=128, DOSR=1024, J=4)
    regs = dict((r, v) for r, v, _ in c.registers())
    check((regs[0x05] & 0x70) >> 4 == 0, "P = 8 encodes as 000")
    check(regs[0x05] & 0x0F == 0, "R = 16 encodes as 0000")
    check(regs[0x0B] & 0x7F == 0, "NDAC = 128 encodes as 000 0000")
    check(regs[0x0C] & 0x7F == 0, "MDAC = 128 encodes as 000 0000")
    check(regs[0x0D] & 0x03 == 0 and regs[0x0E] == 0,
          "DOSR = 1024 encodes as 00 0000 0000")

    c = Config(31250, P=1, R=2, J=44, NDAC=11, MDAC=2, DOSR=128)
    regs = dict((r, v) for r, v, _ in c.registers())
    check((regs[0x05] & 0x70) >> 4 == 1, "P = 1 encodes as 001")
    check(regs[0x05] & 0x0F == 2, "R = 2 encodes as 0010")
    check(regs[0x06] == 44, "J = 44 encodes as its own value")
    check(regs[0x0B] == 0x80 | 11, "NDAC = 11 with the power bit")
    check(regs[0x0E] == 128, "DOSR = 128 in the LSB register")

    # Powering the PLL up must be a separate write after P, R, J and D are set.
    order = [r for r, _, _ in c.registers()]
    check(order.index(0x06) < order.index(0x05), "J written before PLL enable")
    check(order.count(0x05) == 2, "register 5 written twice, config then enable")
    check(order.index(0x0D) + 1 == order.index(0x0E),
          "DOSR LSB written immediately after the MSB")

    # The clock tree agrees with the datasheet identity.
    for cfg in solve(31250, limit=5) + solve(32000, limit=5):
        check(cfg.codec_clkin == cfg.NDAC * cfg.MDAC * cfg.DOSR * cfg.fs,
              "CODEC_CLKIN = NDAC x MDAC x DOSR x DAC_fS")
        check(cfg.dac_fs == cfg.fs, "solved configurations are exact")
        check(cfg.ok(), "solved configurations pass their own checks")

    # Known-bad cases must be reported as bad, or the checks prove nothing.
    borrowed = dict(P=1, R=2, J=32, NDAC=8, MDAC=2, DOSR=128)
    pm = Config(22256, **borrowed)
    check(not pm.ok(), "pico-mac's configuration is rejected")
    check(pm.pll_clk < PLL_OUT_MIN, "pico-mac fails the PLL minimum")
    fd = Config(49716, **borrowed)
    check(not fd.ok(), "fruitjam-doom's configuration is rejected")
    check(fd.dac_mod_clk > DAC_MOD_CLK_MAX,
          "fruitjam-doom fails the DAC_MOD_CLK maximum")

    # A rate that cannot work must yield nothing rather than something wrong.
    check(solve(8000) == [], "8 kHz has no solution, BCLK below the PLL input "
                             "minimum")

    # There must be at least one setting good at both of our candidate rates.
    check(len(common([31250, 32000])) > 0,
          "one register set covers 31250 and 32000 Hz")

    if failures:
        print("SELF-TEST FAILED")
        for f in failures:
            print("  " + f)
        return 1
    print("self-test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
