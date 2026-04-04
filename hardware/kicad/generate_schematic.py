#!/usr/bin/env python3
"""
KiCad 8 Schematic Generator for Grove Shield (Pi Pico)
Generates .kicad_sch file with proper S-expression format
"""

import uuid
from datetime import datetime

def generate_uuid():
    """Generate UUID for KiCad objects"""
    return str(uuid.uuid4())

def generate_kicad_sch():
    """Generate KiCad 8 schematic file content"""

    sch_uuid = generate_uuid()

    # KiCad 8 S-expression schematic
    content = f"""(kicad_sch
  (version 20231120)
  (generator "eeschema")
  (generator_version "8.0")
  (uuid "{sch_uuid}")
  (paper "A4")

  (title_block
    (title "Grove Shield for Raspberry Pi Pico")
    (date "{datetime.now().strftime('%Y-%m-%d')}")
    (rev "1.0")
    (company "AgriHA Project")
    (comment 1 "I2C Grove connectors + ADC terminal blocks")
    (comment 2 "W5500 SPI pins (GP16-21) reserved - NOT connected on this board")
  )

  (lib_symbols
    (symbol "Connector:Raspberry_Pi_Pico"
      (pin_names (offset 1.016))
      (exclude_from_sim no)
      (in_bom yes)
      (on_board yes)
      (property "Reference" "U"
        (at 0 0 0)
        (effects (font (size 1.27 1.27)))
      )
      (property "Value" "Raspberry_Pi_Pico"
        (at 0 -2.54 0)
        (effects (font (size 1.27 1.27)))
      )
      (symbol "Raspberry_Pi_Pico_0_1"
        (rectangle (start -12.7 25.4) (end 12.7 -25.4)
          (stroke (width 0.254) (type default))
          (fill (type background))
        )
      )
      (symbol "Raspberry_Pi_Pico_1_1"
        (pin power_in line (at -15.24 22.86 0) (length 2.54)
          (name "GP0" (effects (font (size 1.27 1.27))))
          (number "1" (effects (font (size 1.27 1.27))))
        )
        (pin power_in line (at -15.24 20.32 0) (length 2.54)
          (name "GP1" (effects (font (size 1.27 1.27))))
          (number "2" (effects (font (size 1.27 1.27))))
        )
        (pin power_in line (at -15.24 17.78 0) (length 2.54)
          (name "GND" (effects (font (size 1.27 1.27))))
          (number "3" (effects (font (size 1.27 1.27))))
        )
        (pin bidirectional line (at -15.24 15.24 0) (length 2.54)
          (name "GP2" (effects (font (size 1.27 1.27))))
          (number "4" (effects (font (size 1.27 1.27))))
        )
        (pin bidirectional line (at -15.24 12.7 0) (length 2.54)
          (name "GP3" (effects (font (size 1.27 1.27))))
          (number "5" (effects (font (size 1.27 1.27))))
        )
        (pin bidirectional line (at -15.24 10.16 0) (length 2.54)
          (name "GP4/SDA0" (effects (font (size 1.27 1.27))))
          (number "6" (effects (font (size 1.27 1.27))))
        )
        (pin bidirectional line (at -15.24 7.62 0) (length 2.54)
          (name "GP5/SCL0" (effects (font (size 1.27 1.27))))
          (number "7" (effects (font (size 1.27 1.27))))
        )
        (pin power_in line (at -15.24 5.08 0) (length 2.54)
          (name "GND" (effects (font (size 1.27 1.27))))
          (number "8" (effects (font (size 1.27 1.27))))
        )
        (pin bidirectional line (at -15.24 2.54 0) (length 2.54)
          (name "GP6/SDA1" (effects (font (size 1.27 1.27))))
          (number "9" (effects (font (size 1.27 1.27))))
        )
        (pin bidirectional line (at -15.24 0 0) (length 2.54)
          (name "GP7/SCL1" (effects (font (size 1.27 1.27))))
          (number "10" (effects (font (size 1.27 1.27))))
        )
        (pin bidirectional line (at 15.24 22.86 180) (length 2.54)
          (name "GP16/SPI_RX" (effects (font (size 1.27 1.27))))
          (number "21" (effects (font (size 1.27 1.27))))
        )
        (pin bidirectional line (at 15.24 20.32 180) (length 2.54)
          (name "GP17/SPI_CSn" (effects (font (size 1.27 1.27))))
          (number "22" (effects (font (size 1.27 1.27))))
        )
        (pin power_in line (at 15.24 15.24 180) (length 2.54)
          (name "3V3" (effects (font (size 1.27 1.27))))
          (number "36" (effects (font (size 1.27 1.27))))
        )
        (pin input line (at -15.24 -10.16 0) (length 2.54)
          (name "GP26/ADC0" (effects (font (size 1.27 1.27))))
          (number "31" (effects (font (size 1.27 1.27))))
        )
        (pin input line (at -15.24 -12.7 0) (length 2.54)
          (name "GP27/ADC1" (effects (font (size 1.27 1.27))))
          (number "32" (effects (font (size 1.27 1.27))))
        )
        (pin input line (at -15.24 -17.78 0) (length 2.54)
          (name "GP28/ADC2" (effects (font (size 1.27 1.27))))
          (number "34" (effects (font (size 1.27 1.27))))
        )
      )
    )

    (symbol "Connector:Grove_I2C_4Pin"
      (pin_names (offset 1.016))
      (exclude_from_sim no)
      (in_bom yes)
      (on_board yes)
      (property "Reference" "J"
        (at 0 0 0)
        (effects (font (size 1.27 1.27)))
      )
      (property "Value" "Grove_I2C"
        (at 0 -2.54 0)
        (effects (font (size 1.27 1.27)))
      )
      (symbol "Grove_I2C_4Pin_0_1"
        (rectangle (start -5.08 5.08) (end 5.08 -5.08)
          (stroke (width 0.254) (type default))
          (fill (type background))
        )
      )
      (symbol "Grove_I2C_4Pin_1_1"
        (pin power_in line (at -7.62 2.54 0) (length 2.54)
          (name "GND" (effects (font (size 1.27 1.27))))
          (number "1" (effects (font (size 1.27 1.27))))
        )
        (pin power_in line (at -7.62 0 0) (length 2.54)
          (name "VCC" (effects (font (size 1.27 1.27))))
          (number "2" (effects (font (size 1.27 1.27))))
        )
        (pin bidirectional line (at -7.62 -2.54 0) (length 2.54)
          (name "SDA" (effects (font (size 1.27 1.27))))
          (number "3" (effects (font (size 1.27 1.27))))
        )
        (pin bidirectional line (at -7.62 -5.08 0) (length 2.54)
          (name "SCL" (effects (font (size 1.27 1.27))))
          (number "4" (effects (font (size 1.27 1.27))))
        )
      )
    )

    (symbol "Device:R"
      (pin_numbers hide)
      (pin_names (offset 0))
      (exclude_from_sim no)
      (in_bom yes)
      (on_board yes)
      (property "Reference" "R"
        (at 2.032 0 90)
        (effects (font (size 1.27 1.27)))
      )
      (property "Value" "R"
        (at 0 0 90)
        (effects (font (size 1.27 1.27)))
      )
      (symbol "R_0_1"
        (rectangle (start -1.016 -2.54) (end 1.016 2.54)
          (stroke (width 0.254) (type default))
          (fill (type none))
        )
      )
      (symbol "R_1_1"
        (pin passive line (at 0 3.81 270) (length 1.27)
          (name "~" (effects (font (size 1.27 1.27))))
          (number "1" (effects (font (size 1.27 1.27))))
        )
        (pin passive line (at 0 -3.81 90) (length 1.27)
          (name "~" (effects (font (size 1.27 1.27))))
          (number "2" (effects (font (size 1.27 1.27))))
        )
      )
    )

    (symbol "Device:C"
      (pin_numbers hide)
      (pin_names (offset 0.254))
      (exclude_from_sim no)
      (in_bom yes)
      (on_board yes)
      (property "Reference" "C"
        (at 0.635 2.54 0)
        (effects (font (size 1.27 1.27)) (justify left))
      )
      (property "Value" "C"
        (at 0.635 -2.54 0)
        (effects (font (size 1.27 1.27)) (justify left))
      )
      (symbol "C_0_1"
        (polyline
          (pts (xy -2.032 -0.762) (xy 2.032 -0.762))
          (stroke (width 0.508) (type default))
          (fill (type none))
        )
        (polyline
          (pts (xy -2.032 0.762) (xy 2.032 0.762))
          (stroke (width 0.508) (type default))
          (fill (type none))
        )
      )
      (symbol "C_1_1"
        (pin passive line (at 0 3.81 270) (length 2.794)
          (name "~" (effects (font (size 1.27 1.27))))
          (number "1" (effects (font (size 1.27 1.27))))
        )
        (pin passive line (at 0 -3.81 90) (length 2.794)
          (name "~" (effects (font (size 1.27 1.27))))
          (number "2" (effects (font (size 1.27 1.27))))
        )
      )
    )

    (symbol "Device:Q_NMOS_GSD"
      (pin_names (offset 0) hide)
      (exclude_from_sim no)
      (in_bom yes)
      (on_board yes)
      (property "Reference" "Q"
        (at 5.08 1.27 0)
        (effects (font (size 1.27 1.27)) (justify left))
      )
      (property "Value" "Q_NMOS_GSD"
        (at 5.08 -1.27 0)
        (effects (font (size 1.27 1.27)) (justify left))
      )
      (symbol "Q_NMOS_GSD_0_1"
        (polyline
          (pts (xy 0.254 0) (xy -2.54 0))
          (stroke (width 0) (type default))
          (fill (type none))
        )
        (polyline
          (pts (xy 0.254 1.905) (xy 0.254 -1.905))
          (stroke (width 0.254) (type default))
          (fill (type none))
        )
        (polyline
          (pts (xy 0.762 -1.27) (xy 0.762 -2.286))
          (stroke (width 0.254) (type default))
          (fill (type none))
        )
        (polyline
          (pts (xy 0.762 0.508) (xy 0.762 -0.508))
          (stroke (width 0.254) (type default))
          (fill (type none))
        )
        (polyline
          (pts (xy 0.762 2.286) (xy 0.762 1.27))
          (stroke (width 0.254) (type default))
          (fill (type none))
        )
        (polyline
          (pts (xy 2.54 2.54) (xy 2.54 1.778))
          (stroke (width 0) (type default))
          (fill (type none))
        )
        (polyline
          (pts (xy 2.54 -2.54) (xy 2.54 0) (xy 0.762 0))
          (stroke (width 0) (type default))
          (fill (type none))
        )
        (polyline
          (pts (xy 0.762 -1.778) (xy 3.302 -1.778) (xy 3.302 1.778) (xy 0.762 1.778))
          (stroke (width 0) (type default))
          (fill (type none))
        )
        (polyline
          (pts (xy 1.016 0) (xy 2.032 0.381) (xy 2.032 -0.381) (xy 1.016 0))
          (stroke (width 0) (type default))
          (fill (type outline))
        )
        (polyline
          (pts (xy 2.794 0.508) (xy 2.921 0.381) (xy 3.683 0.381) (xy 3.81 0.254))
          (stroke (width 0) (type default))
          (fill (type none))
        )
        (polyline
          (pts (xy 3.302 0.381) (xy 2.921 -0.254) (xy 3.683 -0.254) (xy 3.302 0.381))
          (stroke (width 0) (type default))
          (fill (type none))
        )
        (circle (center 1.651 0) (radius 2.794)
          (stroke (width 0.254) (type default))
          (fill (type none))
        )
        (circle (center 2.54 -1.778) (radius 0.254)
          (stroke (width 0) (type default))
          (fill (type outline))
        )
        (circle (center 2.54 1.778) (radius 0.254)
          (stroke (width 0) (type default))
          (fill (type outline))
        )
      )
      (symbol "Q_NMOS_GSD_1_1"
        (pin input line (at -5.08 0 0) (length 2.54)
          (name "G" (effects (font (size 1.27 1.27))))
          (number "1" (effects (font (size 1.27 1.27))))
        )
        (pin passive line (at 2.54 -5.08 90) (length 2.54)
          (name "S" (effects (font (size 1.27 1.27))))
          (number "2" (effects (font (size 1.27 1.27))))
        )
        (pin passive line (at 2.54 5.08 270) (length 2.54)
          (name "D" (effects (font (size 1.27 1.27))))
          (number "3" (effects (font (size 1.27 1.27))))
        )
      )
    )

    (symbol "Device:D"
      (pin_numbers hide)
      (pin_names (offset 1.016) hide)
      (exclude_from_sim no)
      (in_bom yes)
      (on_board yes)
      (property "Reference" "D"
        (at 0 2.54 0)
        (effects (font (size 1.27 1.27)))
      )
      (property "Value" "D"
        (at 0 -2.54 0)
        (effects (font (size 1.27 1.27)))
      )
      (symbol "D_0_1"
        (polyline
          (pts (xy -1.27 1.27) (xy -1.27 -1.27))
          (stroke (width 0.254) (type default))
          (fill (type none))
        )
        (polyline
          (pts (xy 1.27 0) (xy -1.27 0))
          (stroke (width 0) (type default))
          (fill (type none))
        )
        (polyline
          (pts (xy 1.27 1.27) (xy 1.27 -1.27) (xy -1.27 0) (xy 1.27 1.27))
          (stroke (width 0.254) (type default))
          (fill (type none))
        )
      )
      (symbol "D_1_1"
        (pin passive line (at -3.81 0 0) (length 2.54)
          (name "K" (effects (font (size 1.27 1.27))))
          (number "1" (effects (font (size 1.27 1.27))))
        )
        (pin passive line (at 3.81 0 180) (length 2.54)
          (name "A" (effects (font (size 1.27 1.27))))
          (number "2" (effects (font (size 1.27 1.27))))
        )
      )
    )

    (symbol "Connector:Conn_01x02"
      (pin_names (offset 1.016) hide)
      (exclude_from_sim no)
      (in_bom yes)
      (on_board yes)
      (property "Reference" "J"
        (at 0 2.54 0)
        (effects (font (size 1.27 1.27)))
      )
      (property "Value" "Conn_01x02"
        (at 0 -5.08 0)
        (effects (font (size 1.27 1.27)))
      )
      (symbol "Conn_01x02_1_1"
        (rectangle (start -1.27 -2.413) (end 0 -2.667)
          (stroke (width 0.1524) (type default))
          (fill (type none))
        )
        (rectangle (start -1.27 0.127) (end 0 -0.127)
          (stroke (width 0.1524) (type default))
          (fill (type none))
        )
        (rectangle (start -1.27 1.27) (end 1.27 -3.81)
          (stroke (width 0.254) (type default))
          (fill (type background))
        )
        (pin passive line (at -5.08 0 0) (length 3.81)
          (name "Pin_1" (effects (font (size 1.27 1.27))))
          (number "1" (effects (font (size 1.27 1.27))))
        )
        (pin passive line (at -5.08 -2.54 0) (length 3.81)
          (name "Pin_2" (effects (font (size 1.27 1.27))))
          (number "2" (effects (font (size 1.27 1.27))))
        )
      )
    )
  )

  (symbol
    (lib_id "Connector:Raspberry_Pi_Pico")
    (at 50 100 0)
    (unit 1)
    (exclude_from_sim no)
    (in_bom yes)
    (on_board yes)
    (dnp no)
    (uuid "{generate_uuid()}")
    (property "Reference" "U1"
      (at 50 130 0)
      (effects (font (size 1.27 1.27)))
    )
    (property "Value" "Raspberry_Pi_Pico"
      (at 50 127 0)
      (effects (font (size 1.27 1.27)))
    )
  )

  (symbol
    (lib_id "Connector:Grove_I2C_4Pin")
    (at 120 80 0)
    (unit 1)
    (exclude_from_sim no)
    (in_bom yes)
    (on_board yes)
    (dnp no)
    (uuid "{generate_uuid()}")
    (property "Reference" "J3"
      (at 120 87 0)
      (effects (font (size 1.27 1.27)))
    )
    (property "Value" "Grove_I2C0"
      (at 120 85 0)
      (effects (font (size 1.27 1.27)))
    )
  )

  (symbol
    (lib_id "Connector:Grove_I2C_4Pin")
    (at 120 100 0)
    (unit 1)
    (exclude_from_sim no)
    (in_bom yes)
    (on_board yes)
    (dnp no)
    (uuid "{generate_uuid()}")
    (property "Reference" "J4"
      (at 120 107 0)
      (effects (font (size 1.27 1.27)))
    )
    (property "Value" "Grove_I2C1"
      (at 120 105 0)
      (effects (font (size 1.27 1.27)))
    )
  )

  (symbol
    (lib_id "Connector:Conn_01x03")
    (at 120 120 0)
    (unit 1)
    (exclude_from_sim no)
    (in_bom yes)
    (on_board yes)
    (dnp no)
    (uuid "{generate_uuid()}")
    (property "Reference" "J8"
      (at 120 127 0)
      (effects (font (size 1.27 1.27)))
    )
    (property "Value" "1-Wire_DS18B20"
      (at 120 125 0)
      (effects (font (size 1.27 1.27)))
    )
  )

  (symbol
    (lib_id "Device:R")
    (at 90 75 0)
    (unit 1)
    (exclude_from_sim no)
    (in_bom yes)
    (on_board yes)
    (dnp no)
    (uuid "{generate_uuid()}")
    (property "Reference" "R1"
      (at 92 75 0)
      (effects (font (size 1.27 1.27)) (justify left))
    )
    (property "Value" "4.7k"
      (at 92 77 0)
      (effects (font (size 1.27 1.27)) (justify left))
    )
  )

  (symbol
    (lib_id "Device:R")
    (at 95 75 0)
    (unit 1)
    (exclude_from_sim no)
    (in_bom yes)
    (on_board yes)
    (dnp no)
    (uuid "{generate_uuid()}")
    (property "Reference" "R2"
      (at 97 75 0)
      (effects (font (size 1.27 1.27)) (justify left))
    )
    (property "Value" "4.7k"
      (at 97 77 0)
      (effects (font (size 1.27 1.27)) (justify left))
    )
  )

  (symbol
    (lib_id "Device:R")
    (at 90 95 0)
    (unit 1)
    (exclude_from_sim no)
    (in_bom yes)
    (on_board yes)
    (dnp no)
    (uuid "{generate_uuid()}")
    (property "Reference" "R3"
      (at 92 95 0)
      (effects (font (size 1.27 1.27)) (justify left))
    )
    (property "Value" "4.7k"
      (at 92 97 0)
      (effects (font (size 1.27 1.27)) (justify left))
    )
  )

  (symbol
    (lib_id "Device:R")
    (at 95 95 0)
    (unit 1)
    (exclude_from_sim no)
    (in_bom yes)
    (on_board yes)
    (dnp no)
    (uuid "{generate_uuid()}")
    (property "Reference" "R4"
      (at 97 95 0)
      (effects (font (size 1.27 1.27)) (justify left))
    )
    (property "Value" "4.7k"
      (at 97 97 0)
      (effects (font (size 1.27 1.27)) (justify left))
    )
  )

  (symbol
    (lib_id "Device:R")
    (at 90 110 0)
    (unit 1)
    (exclude_from_sim no)
    (in_bom yes)
    (on_board yes)
    (dnp no)
    (uuid "{generate_uuid()}")
    (property "Reference" "R5"
      (at 92 110 0)
      (effects (font (size 1.27 1.27)) (justify left))
    )
    (property "Value" "4.7k"
      (at 92 112 0)
      (effects (font (size 1.27 1.27)) (justify left))
    )
  )

  (symbol
    (lib_id "Device:C")
    (at 105 82 0)
    (unit 1)
    (exclude_from_sim no)
    (in_bom yes)
    (on_board yes)
    (dnp no)
    (uuid "{generate_uuid()}")
    (property "Reference" "C1"
      (at 107 80 0)
      (effects (font (size 1.27 1.27)) (justify left))
    )
    (property "Value" "100nF"
      (at 107 84 0)
      (effects (font (size 1.27 1.27)) (justify left))
    )
  )

  (symbol
    (lib_id "Device:C")
    (at 105 102 0)
    (unit 1)
    (exclude_from_sim no)
    (in_bom yes)
    (on_board yes)
    (dnp no)
    (uuid "{generate_uuid()}")
    (property "Reference" "C2"
      (at 107 100 0)
      (effects (font (size 1.27 1.27)) (justify left))
    )
    (property "Value" "100nF"
      (at 107 104 0)
      (effects (font (size 1.27 1.27)) (justify left))
    )
  )

  (symbol
    (lib_id "Device:C")
    (at 105 130 0)
    (unit 1)
    (exclude_from_sim no)
    (in_bom yes)
    (on_board yes)
    (dnp no)
    (uuid "{generate_uuid()}")
    (property "Reference" "C3"
      (at 107 128 0)
      (effects (font (size 1.27 1.27)) (justify left))
    )
    (property "Value" "100nF"
      (at 107 132 0)
      (effects (font (size 1.27 1.27)) (justify left))
    )
  )

  (symbol
    (lib_id "Device:Q_NMOS_GSD")
    (at 140 110 0)
    (unit 1)
    (exclude_from_sim no)
    (in_bom yes)
    (on_board yes)
    (dnp no)
    (uuid "{generate_uuid()}")
    (property "Reference" "Q1"
      (at 145 108 0)
      (effects (font (size 1.27 1.27)) (justify left))
    )
    (property "Value" "2N7002"
      (at 145 112 0)
      (effects (font (size 1.27 1.27)) (justify left))
    )
  )

  (symbol
    (lib_id "Device:R")
    (at 130 110 0)
    (unit 1)
    (exclude_from_sim no)
    (in_bom yes)
    (on_board yes)
    (dnp no)
    (uuid "{generate_uuid()}")
    (property "Reference" "R6"
      (at 132 110 0)
      (effects (font (size 1.27 1.27)) (justify left))
    )
    (property "Value" "100"
      (at 132 112 0)
      (effects (font (size 1.27 1.27)) (justify left))
    )
  )

  (symbol
    (lib_id "Device:R")
    (at 135 115 0)
    (unit 1)
    (exclude_from_sim no)
    (in_bom yes)
    (on_board yes)
    (dnp no)
    (uuid "{generate_uuid()}")
    (property "Reference" "R7"
      (at 137 115 0)
      (effects (font (size 1.27 1.27)) (justify left))
    )
    (property "Value" "10k"
      (at 137 117 0)
      (effects (font (size 1.27 1.27)) (justify left))
    )
  )

  (symbol
    (lib_id "Device:D")
    (at 150 105 0)
    (unit 1)
    (exclude_from_sim no)
    (in_bom yes)
    (on_board yes)
    (dnp no)
    (uuid "{generate_uuid()}")
    (property "Reference" "D1"
      (at 150 107 0)
      (effects (font (size 1.27 1.27)))
    )
    (property "Value" "1N4148"
      (at 150 103 0)
      (effects (font (size 1.27 1.27)))
    )
  )

  (symbol
    (lib_id "Connector:Conn_01x02")
    (at 160 105 0)
    (unit 1)
    (exclude_from_sim no)
    (in_bom yes)
    (on_board yes)
    (dnp no)
    (uuid "{generate_uuid()}")
    (property "Reference" "J9"
      (at 160 108 0)
      (effects (font (size 1.27 1.27)))
    )
    (property "Value" "Fan_30mm_5V"
      (at 160 101 0)
      (effects (font (size 1.27 1.27)))
    )
  )

  (text "W5500 SPI RESERVED PINS (GP16-21):\\nGP16: SPI0_RX (MISO)\\nGP17: SPI0_CSn\\nGP18: SPI0_SCK\\nGP19: SPI0_TX (MOSI)\\nGP20: W5500_INT\\nGP21: W5500_RST\\n\\nNOTE: These pins are NOT connected on this board.\\nReserved for W5500-EVB-Pico-PoE shield."
    (exclude_from_sim no)
    (at 130 50 0)
    (effects
      (font (size 1.27 1.27))
      (justify left)
    )
    (uuid "{generate_uuid()}")
  )

  (text "ADC Terminal Blocks:\\nJ5: GP26_ADC0 (GND, SIG, 3V3)\\nJ6: GP27_ADC1 (GND, SIG, 3V3)\\nJ7: GP28_ADC2 (GND, SIG, 3V3)"
    (exclude_from_sim no)
    (at 130 120 0)
    (effects
      (font (size 1.27 1.27))
      (justify left)
    )
    (uuid "{generate_uuid()}")
  )

  (text "1-Wire DS18B20 Connector:\\nJ8: GP2_1WIRE (GND, DQ=GP2, 3V3)\\nR5: 4.7kΩ pull-up (DQ → 3V3)"
    (exclude_from_sim no)
    (at 130 135 0)
    (effects
      (font (size 1.27 1.27))
      (justify left)
    )
    (uuid "{generate_uuid()}")
  )

  (text "Fan Drive Circuit (30mm 5V Fan):\\nJ9: Fan Connector (VSYS, Drain)\\nQ1: N-ch MOSFET 2N7002 (SOT-23)\\nR6: Gate resistor 100Ω (GP22 → Gate)\\nR7: Pull-down 10kΩ (Gate → GND)\\nD1: Flyback diode 1N4148\\nPWM: GP22 (25kHz recommended)"
    (exclude_from_sim no)
    (at 130 150 0)
    (effects
      (font (size 1.27 1.27))
      (justify left)
    )
    (uuid "{generate_uuid()}")
  )

  (text "ADC_VREF Decoupling:\\nC3: 100nF (ADC_VREF pin35 → GND)"
    (exclude_from_sim no)
    (at 130 165 0)
    (effects
      (font (size 1.27 1.27))
      (justify left)
    )
    (uuid "{generate_uuid()}")
  )

  (sheet_instances
    (path "/"
      (page "1")
    )
  )
)
"""

    return content

def main():
    """Main function"""
    print("Generating KiCad 8 schematic file...")

    # Generate .kicad_sch file
    sch_content = generate_kicad_sch()
    sch_file = "/home/yasu/unipi-agri-ha/hardware/kicad/grove_shield.kicad_sch"

    with open(sch_file, "w", encoding="utf-8") as f:
        f.write(sch_content)

    print(f"✓ Generated: {sch_file}")

    # Generate .kicad_pro file (minimal KiCad 8 project)
    pro_content = """{
  "board": {
    "3dviewports": [],
    "design_settings": {
      "defaults": {
        "board_outline_line_width": 0.1,
        "copper_line_width": 0.2,
        "copper_text_size_h": 1.5,
        "copper_text_size_v": 1.5,
        "copper_text_thickness": 0.3,
        "other_line_width": 0.15,
        "silk_line_width": 0.15,
        "silk_text_size_h": 1.0,
        "silk_text_size_v": 1.0,
        "silk_text_thickness": 0.15
      },
      "diff_pair_dimensions": [],
      "drc_exclusions": [],
      "rules": {
        "min_copper_edge_clearance": 0.0,
        "solder_mask_clearance": 0.0,
        "solder_mask_min_width": 0.0
      },
      "track_widths": [],
      "via_dimensions": []
    }
  },
  "boards": [],
  "cvpcb": {
    "equivalence_files": []
  },
  "erc": {
    "erc_exclusions": [],
    "pin_map": [
      [0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2],
      [0, 2, 0, 1, 0, 0, 1, 0, 2, 2, 2, 2],
      [0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 2],
      [0, 1, 0, 0, 0, 0, 1, 1, 2, 1, 1, 2],
      [0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2],
      [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2],
      [1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 2],
      [0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 2],
      [0, 2, 1, 2, 0, 0, 1, 0, 2, 2, 2, 2],
      [0, 2, 0, 1, 0, 0, 1, 0, 2, 0, 0, 2],
      [0, 2, 1, 1, 0, 0, 1, 0, 2, 0, 0, 2],
      [2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2]
    ],
    "rule_severities": {
      "bus_definition_conflict": "error",
      "bus_entry_needed": "error",
      "bus_to_bus_conflict": "error",
      "bus_to_net_conflict": "error",
      "conflicting_netclasses": "error",
      "different_unit_footprint": "error",
      "different_unit_net": "error",
      "duplicate_reference": "error",
      "duplicate_sheet_names": "error",
      "endpoint_off_grid": "warning",
      "extra_units": "error",
      "global_label_dangling": "warning",
      "hier_label_mismatch": "error",
      "label_dangling": "error",
      "lib_symbol_issues": "warning",
      "missing_bidi_pin": "warning",
      "missing_input_pin": "warning",
      "missing_power_pin": "error",
      "missing_unit": "warning",
      "multiple_net_names": "warning",
      "net_not_bus_member": "warning",
      "no_connect_connected": "warning",
      "no_connect_dangling": "warning",
      "pin_not_connected": "error",
      "pin_not_driven": "error",
      "pin_to_pin": "warning",
      "power_pin_not_driven": "error",
      "similar_labels": "warning",
      "simulation_model_issue": "ignore",
      "unannotated": "error",
      "unit_value_mismatch": "error",
      "unresolved_variable": "error",
      "wire_dangling": "error"
    }
  },
  "libraries": {
    "pinned_footprint_libs": [],
    "pinned_symbol_libs": []
  },
  "meta": {
    "filename": "grove_shield.kicad_pro",
    "version": 1
  },
  "net_settings": {
    "classes": [
      {
        "bus_width": 12,
        "clearance": 0.2,
        "diff_pair_gap": 0.25,
        "diff_pair_via_gap": 0.25,
        "diff_pair_width": 0.2,
        "line_style": 0,
        "microvia_diameter": 0.3,
        "microvia_drill": 0.1,
        "name": "Default",
        "pcb_color": "rgba(0, 0, 0, 0.000)",
        "schematic_color": "rgba(0, 0, 0, 0.000)",
        "track_width": 0.25,
        "via_diameter": 0.8,
        "via_drill": 0.4,
        "wire_width": 6
      }
    ],
    "meta": {
      "version": 3
    },
    "net_colors": null,
    "netclass_assignments": null,
    "netclass_patterns": []
  },
  "pcbnew": {
    "last_paths": {
      "gencad": "",
      "idf": "",
      "netlist": "",
      "specctra_dsn": "",
      "step": "",
      "vrml": ""
    },
    "page_layout_descr_file": ""
  },
  "schematic": {
    "annotate_start_num": 0,
    "drawing": {
      "dashed_lines_dash_length_ratio": 12.0,
      "dashed_lines_gap_length_ratio": 3.0,
      "default_line_thickness": 6.0,
      "default_text_size": 50.0,
      "field_names": [],
      "intersheets_ref_own_page": false,
      "intersheets_ref_prefix": "",
      "intersheets_ref_short": false,
      "intersheets_ref_show": false,
      "intersheets_ref_suffix": "",
      "junction_size_choice": 3,
      "label_size_ratio": 0.375,
      "pin_symbol_size": 25.0,
      "text_offset_ratio": 0.15
    },
    "legacy_lib_dir": "",
    "legacy_lib_list": [],
    "meta": {
      "version": 1
    },
    "net_format_name": "",
    "page_layout_descr_file": "",
    "plot_directory": "",
    "spice_current_sheet_as_root": false,
    "spice_external_command": "spice \\\"%I\\\"",
    "spice_model_current_sheet_as_root": true,
    "spice_save_all_currents": false,
    "spice_save_all_voltages": false,
    "subpart_first_id": 65,
    "subpart_id_separator": 0
  },
  "sheets": [
    [
      "00000000-0000-0000-0000-000000000000",
      ""
    ]
  ],
  "text_variables": {}
}
"""

    pro_file = "/home/yasu/unipi-agri-ha/hardware/kicad/grove_shield.kicad_pro"
    with open(pro_file, "w", encoding="utf-8") as f:
        f.write(pro_content)

    print(f"✓ Generated: {pro_file}")
    print("\nKiCad 8 project generated successfully!")
    print("Open with: kicad /home/yasu/unipi-agri-ha/hardware/kicad/grove_shield.kicad_pro")

if __name__ == "__main__":
    main()
