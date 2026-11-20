#ifndef PERMIT_CONFIG_H
#define PERMIT_CONFIG_H

// ========== SCREEN SETTINGS ==========
const int SCREEN_W = 296;
const int SCREEN_H = 128;
const bool FLIP_DISPLAY = false;  // Set to true to rotate display 180°

// ========== PERMIT DATA ==========
const char *PERMIT_NUMBER = "T6103268";
const char *PLATE_NUMBER = "CSEB187";
const char *VALID_FROM = "Sep 05, 2025: 01:08";
const char *VALID_TO = "Sep 12, 2025: 01:08";
const char *BARCODE_VALUE = "6103268";
const char *BARCODE_LABEL = "00435";

// ========== LAYOUT: RIGHT COLUMN (beside logo, x >= 146) ==========
const int RIGHT_COL_X  = 146;
const int TITLE_Y1     = 14;   // "Temporary parking"
const int TITLE_Y2     = 27;   // "permit"
const int PERMIT_Y     = 49;   // Permit #
const int PLATE_Y      = 62;   // Plate # (inline with DATE_FROM_Y)


// ========== LAYOUT: LEFT COLUMN (below logo) ==========
const int DATE_FROM_Y  = 62;   // Valid from
const int DATE_TO_Y    = 75;   // Valid to

// ========== SEPARATOR ==========
const int SEPARATOR_Y  = 78;

// ========== BARCODE (full width, bottom) ==========
const int BARCODE_X             = 5;
const int BARCODE_Y             = 80;
const int BARCODE_HEIGHT        = 45;
const int NARROW_BAR_WIDTH      = 2;
const int BARCODE_LABEL_Y_OFFSET = 12;  // offset below barcode bottom

#endif
