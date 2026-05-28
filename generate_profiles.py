#!/usr/bin/env python3
"""
Generate profile_xml for all imported players.

XML values range 0.0–0.99, derived from DB attribute columns.
Compound attributes use weighted blends or bonuses as documented in CLAUDE.md.

Run: python3 generate_profiles.py [--dry-run]
"""

import sqlite3
import sys
import argparse
import math
from pathlib import Path

DEFAULT_DB_PATHS = (
    "mestre.sqlite",
    "build/databases/default/database.sqlite",
    "data/databases/default/database.sqlite",
)

XML_TEMPLATE = """\
<physical_balance>{balance}</physical_balance>
<physical_reaction>{reaction}</physical_reaction>
<physical_acceleration>{acceleration}</physical_acceleration>
<physical_velocity>{velocity}</physical_velocity>
<physical_stamina>{stamina}</physical_stamina>
<physical_agility>{agility}</physical_agility>
<physical_shotpower>{shotpower}</physical_shotpower>
<technical_standingtackle>{standingtackle}</technical_standingtackle>
<technical_slidingtackle>{slidingtackle}</technical_slidingtackle>
<technical_ballcontrol>{ballcontrol}</technical_ballcontrol>
<technical_dribble>{dribble}</technical_dribble>
<technical_shortpass>{shortpass}</technical_shortpass>
<technical_highpass>{highpass}</technical_highpass>
<technical_header>{header}</technical_header>
<technical_shot>{shot}</technical_shot>
<technical_volley>{volley}</technical_volley>
<mental_calmness>{calmness}</mental_calmness>
<mental_workrate>{workrate}</mental_workrate>
<mental_resilience>{resilience}</mental_resilience>
<mental_defensivepositioning>{defensivepositioning}</mental_defensivepositioning>
<mental_offensivepositioning>{offensivepositioning}</mental_offensivepositioning>
<mental_vision>{vision}</mental_vision>"""


def clamp(value: float) -> float:
    """Clamp to 0–99 range, then convert to 0.00–0.99 format."""
    return round(max(0.0, min(99.0, value)) / 100, 2)


def curve(source: float) -> float:
    """Match src/base/math/bluntmath.hpp curve(source, 1.0)."""
    return math.sin((source - 0.5) * math.pi) * 0.5 + 0.5


def aged_base_stat(base_stat: float, age: int) -> float:
    """Match src/utils.cpp CalculateStat() base scaling."""
    ideal_age = 27
    age_factor = curve(1.0 - min(abs(age - ideal_age) / 13.0, 1.0) * 0.5) * 2.0 - 1.0
    return base_stat * (age_factor * 0.5 + 0.5) * 1.2


def profile_value_for_target(target_0_99: float, base_stat: float, age: int) -> float:
    """
    profile_xml is a relative profile, not the final stat.

    The engine calculates:
      final_stat = profile_xml_value * 2.0 * aged_base_stat

    Backsolve that so an imported 83 finishing becomes ~0.83 in engine terms,
    instead of high-base players and decent-base players both slamming into 1.0.
    """
    target = max(1.0, min(99.0, target_0_99)) / 100.0
    scale = 2.0 * aged_base_stat(base_stat, age)
    if scale <= 0.0:
      return clamp(target_0_99)
    return round(max(0.01, min(0.99, target / scale)), 2)


def build_profile(p: dict) -> str:
    height_m  = p["height"] or 1.78
    height    = height_m * 100       # convert to cm for bonus calculations
    base_stat = p["base_stat"] or 0.6
    age       = p["age"] or 27

    def xml(value: float) -> float:
        return profile_value_for_target(value, base_stat, age)

    # --- Physical ---
    balance      = xml(p["Balance"])
    reaction     = xml(p["Reactions"])
    acceleration = xml(p["Acceleration"])
    velocity     = xml(p["SprintSpeed"])
    stamina      = xml(p["stamina"])
    agility      = xml(p["Agility"])
    shotpower    = xml(p["ShotPower"])

    # --- Technical ---
    standingtackle = xml(p["StandingTackle"])
    slidingtackle  = xml(p["SlidingTackle"])

    # BallControl: no bonus (skillMoves bonus goes to dribble only)
    ballcontrol = xml(p["BallControl"])

    # Dribble: base Dribbling + skillMoves bonus (each level above 1 adds 3 pts, max +12)
    sm = p["skillMoves"] or 1
    dribble = xml(p["Dribbling"] + (sm - 1) * 3)

    shortpass = xml(p["ShortPassing"])
    highpass  = xml(p["LongPassing"])

    # Header: HeadingAccuracy base + height bonus (each cm above 175) + jumping bonus
    height_bonus = max(0.0, (height - 175.0)) * 0.3
    jump_bonus   = (p["Jumping"] / 99.0) * 10.0
    header = xml(p["HeadingAccuracy"] + height_bonus + jump_bonus)

    # Shot: finishing-led. Curve matters, but should not rival finishing.
    shot = xml(p["Finishing"] * 0.65 + p["LongShots"] * 0.25 + p["Curve"] * 0.10)

    volley = xml(p["Volleys"])

    # --- Mental ---
    # Calmness should reflect composure, not inverse aggression.
    calmness = xml(p["Composure"])

    # No direct work-rate column exists in this DB, so blend effort-like traits.
    workrate = xml(p["stamina"] * 0.45 + p["Aggression"] * 0.30 + p["Reactions"] * 0.25)

    # Resilience driven by Strength
    resilience = xml(p["Strength"])

    defensivepositioning  = xml(p["DefensiveAwareness"])
    offensivepositioning  = xml(p["Positioning"])
    vision                = xml(p["Vision"])

    return XML_TEMPLATE.format(
        balance=balance,
        reaction=reaction,
        acceleration=acceleration,
        velocity=velocity,
        stamina=stamina,
        agility=agility,
        shotpower=shotpower,
        standingtackle=standingtackle,
        slidingtackle=slidingtackle,
        ballcontrol=ballcontrol,
        dribble=dribble,
        shortpass=shortpass,
        highpass=highpass,
        header=header,
        shot=shot,
        volley=volley,
        calmness=calmness,
        workrate=workrate,
        resilience=resilience,
        defensivepositioning=defensivepositioning,
        offensivepositioning=offensivepositioning,
        vision=vision,
    )


COLUMNS = [
    "id", "nickname", "height", "age", "base_stat",
    "Balance", "Reactions", "Acceleration", "SprintSpeed", "stamina",
    "Agility", "ShotPower", "StandingTackle", "SlidingTackle",
    "BallControl", "Dribbling", "skillMoves",
    "ShortPassing", "LongPassing", "HeadingAccuracy", "Jumping",
    "Finishing", "LongShots", "Curve", "Volleys",
    "Aggression", "Composure", "Strength",
    "DefensiveAwareness", "Positioning", "Vision",
]


def resolve_db_path(cli_path: str | None) -> Path:
    candidates = [cli_path] if cli_path else DEFAULT_DB_PATHS
    for candidate in candidates:
        if not candidate:
            continue
        path = Path(candidate)
        if path.exists() and path.stat().st_size > 0:
            return path
    searched = ", ".join(str(p) for p in candidates)
    raise FileNotFoundError(f"No non-empty database found. Searched: {searched}")


def main():
    parser = argparse.ArgumentParser(description="Generate engine profile_xml from imported player attributes.")
    parser.add_argument("--db", default=None, help="SQLite database path")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    db_path = resolve_db_path(args.db)
    print(f"Using database: {db_path}")

    con = sqlite3.connect(db_path)
    con.row_factory = sqlite3.Row
    cur = con.cursor()

    rows = cur.execute(
        f"SELECT {', '.join(COLUMNS)} FROM players WHERE (futggId IS NOT NULL OR sofifaId IS NOT NULL)"
    ).fetchall()

    skipped = 0
    updated = 0

    for row in rows:
        p = dict(row)
        name = p["nickname"] or str(p["id"])

        # Skip if any required attribute is NULL
        missing = [c for c in COLUMNS[2:] if p[c] is None]
        if missing:
            print(f"  SKIP {name} — missing: {missing}")
            skipped += 1
            continue

        xml = build_profile(p)

        if args.dry_run and updated < 3:
            print(f"\n--- {name} ---\n{xml}\n")

        if not args.dry_run:
            cur.execute(
                "UPDATE players SET profile_xml = ? WHERE id = ?",
                (xml, p["id"]),
            )
        updated += 1

    if not args.dry_run:
        con.commit()

    con.close()
    print(f"\n{'Would update' if args.dry_run else 'Updated'} {updated} players  |  Skipped {skipped}")


if __name__ == "__main__":
    main()
