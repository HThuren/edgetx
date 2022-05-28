/*
 * Copyright (C) EdgeTX
 *
 * Based on code named
 *   opentx - https://github.com/opentx/opentx
 *   th9x - http://code.google.com/p/th9x
 *   er9x - http://code.google.com/p/er9x
 *   gruvin9x - http://code.google.com/p/gruvin9x
 *
 * License GPLv2: http://www.gnu.org/licenses/gpl-2.0.html
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "opentx.h"

#include "LvglWrapper.h"
#include "themes/etx_lv_theme.h"
#include "view_main.h"

LvglWrapper* LvglWrapper::_instance = nullptr;

static lv_indev_drv_t touchDriver;
static lv_indev_drv_t keyboard_drv;
static lv_indev_drv_t rotaryDriver;

static lv_indev_t* rotaryDevice = nullptr;
static lv_indev_t* keyboardDevice = nullptr;

static lv_obj_t* get_focus_obj(lv_indev_t* indev)
{
  lv_group_t * g = indev->group;
  if(g == nullptr) return nullptr;
  return lv_group_get_focused(g);
}

static lv_indev_data_t kb_data_backup;

static void backup_kb_data(lv_indev_data_t* data)
{
  memcpy(&kb_data_backup, data, sizeof(lv_indev_data_t));
}

static void copy_kb_data_backup(lv_indev_data_t* data)
{
  memcpy(data, &kb_data_backup, sizeof(lv_indev_data_t));
}

constexpr event_t _KEY_PRESSED = _MSK_KEY_FLAGS & ~_MSK_KEY_BREAK;

static bool evt_to_indev_data(event_t evt, lv_indev_data_t *data)
{
  event_t key = EVT_KEY_MASK(evt);
  switch(key) {

  case KEY_ENTER:
    data->key = LV_KEY_ENTER;
    break;

  case KEY_EXIT:
    data->key = LV_KEY_ESC;
    break;    

  default:
    // abort LVGL event
    return false;
  }

  if (evt & _KEY_PRESSED) {
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }

  return true;
}

static void dispatch_kb_event(Window* w, event_t evt)
{
  if (!w) return;

  event_t key = EVT_KEY_MASK(evt);
  if (evt == EVT_KEY_BREAK(KEY_ENTER)) {
    w->onClicked();
  } else if (evt == EVT_KEY_FIRST(KEY_EXIT)) {
    w->onCancel();
  } else if (key != KEY_ENTER /*&& key != KEY_EXIT*/) {
    w->onEvent(evt);
  }
}

static void keyboardDriverRead(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
  data->key = 0;

  if (isEvent()) {
    event_t evt = getWindowEvent();

    // no focused item ?
    auto obj = get_focus_obj(keyboardDevice);
    if (!obj) {
      auto w = Layer::back();
      dispatch_kb_event(w, evt);
      backup_kb_data(data);
      return;
    }

    // not an LVGL key ?
    bool is_lvgl_evt = evt_to_indev_data(evt, data);
    if (!is_lvgl_evt) {
      auto w = (Window*)lv_obj_get_user_data(obj);
      dispatch_kb_event(w, evt);
      return;
    }

    backup_kb_data(data);
    return;
  }

  // no event: send a copy of the last one
  copy_kb_data_backup(data);
}

static void copy_ts_to_indev_data(const TouchState &st, lv_indev_data_t *data)
{
  data->point.x = st.x;
  data->point.y = st.y;
}

static lv_indev_data_t touch_data_backup;

static void backup_touch_data(lv_indev_data_t* data)
{
  memcpy(&touch_data_backup, data, sizeof(lv_indev_data_t));
}

static void copy_touch_data_backup(lv_indev_data_t* data)
{
  memcpy(data, &touch_data_backup, sizeof(lv_indev_data_t));
}

extern "C" void touchDriverRead(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
#if defined(HARDWARE_TOUCH)
  if(!touchPanelEventOccured()) {
    copy_touch_data_backup(data);
    return;
  }

  TouchState st = touchPanelRead();
  if(st.event == TE_NONE) {
    TRACE("TE_NONE");
  } else if(st.event == TE_DOWN || st.event == TE_SLIDE) {
    TRACE("INDEV_STATE_PRESSED");
    data->state = LV_INDEV_STATE_PRESSED;
    copy_ts_to_indev_data(st, data);
  } else {
    TRACE("INDEV_STATE_RELEASED");
    data->state = LV_INDEV_STATE_RELEASED;
    copy_ts_to_indev_data(st, data);
  }

  backup_touch_data(data);
#endif
}

static void rotaryDriverRead(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
  static rotenc_t prevPos = 0;

  rotenc_t newPos = (ROTARY_ENCODER_NAVIGATION_VALUE / ROTARY_ENCODER_GRANULARITY);
  auto diff = newPos - prevPos;
  prevPos = newPos;

  data->enc_diff = (int16_t)diff;
  data->state = LV_INDEV_STATE_RELEASED;
}

/**
 * Helper function to translate a colorFlags value to a lv_color_t suitable
 * for passing to an lv_obj function
 * @param colorFlags a textFlags value.  This value will contain the color shifted by 16 bits.
 */
lv_color_t makeLvColor(uint32_t colorFlags)
{
  auto color = COLOR_VAL(colorFlags);
  return lv_color_make(GET_RED(color), GET_GREEN(color), GET_BLUE(color));
}

static void init_lvgl_drivers()
{
  // Register the driver and save the created display object
  lcdInitDisplayDriver();
 
  // Register the driver in LVGL and save the created input device object
  lv_indev_drv_init(&touchDriver);          /*Basic initialization*/
  touchDriver.type = LV_INDEV_TYPE_POINTER; /*See below.*/
  touchDriver.read_cb = touchDriverRead;      /*See below.*/
  lv_indev_drv_register(&touchDriver);

  lv_indev_drv_init(&rotaryDriver);
  rotaryDriver.type = LV_INDEV_TYPE_ENCODER;
  rotaryDriver.read_cb = rotaryDriverRead;
  rotaryDevice = lv_indev_drv_register(&rotaryDriver);

  lv_indev_drv_init(&keyboard_drv);
  keyboard_drv.type = LV_INDEV_TYPE_KEYPAD;
  keyboard_drv.read_cb = keyboardDriverRead;
  keyboardDevice = lv_indev_drv_register(&keyboard_drv);
}

// The theme code needs to go somewhere else (gui/colorlcd/themes/default.cpp?)
static lv_style_t menu_line_style;
static lv_style_t menu_checked_style;
static lv_style_t textedit_style_main;
static lv_style_t textedit_style_focused;
static lv_style_t textedit_style_cursor;
static lv_style_t textedit_style_cursor_edit;
static lv_style_t numberedit_style_main;
static lv_style_t numberedit_style_focused;
static lv_style_t numberedit_style_cursor;
static lv_style_t numberedit_style_cursor_edit;


static void theme_apply_cb(lv_theme_t * th, lv_obj_t * obj)
{
  LV_UNUSED(th);

  if (lv_obj_check_type(obj, &lv_obj_class)) {
    // lv_obj_add_style(obj, &generic_style, LV_PART_MAIN);
    return;
  }

  lv_obj_t* parent = lv_obj_get_parent(obj);
  if (parent == NULL) {
    // main screen
    return;
  }

  if (lv_obj_check_type(obj, &lv_numberedit_class)) {
    lv_obj_add_style(obj, &numberedit_style_main, LV_PART_MAIN);
    lv_obj_add_style(obj, &numberedit_style_focused, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_add_style(obj, &numberedit_style_cursor, LV_PART_CURSOR);
    lv_obj_add_style(obj, &numberedit_style_cursor_edit, LV_PART_CURSOR | LV_STATE_EDITED);
    return;
  }

  if (lv_obj_check_type(obj, &lv_textarea_class)) {
    lv_obj_add_style(obj, &textedit_style_main, LV_PART_MAIN);
    lv_obj_add_style(obj, &textedit_style_focused, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_add_style(obj, &textedit_style_cursor, LV_PART_CURSOR);
    lv_obj_add_style(obj, &textedit_style_cursor_edit, LV_PART_CURSOR | LV_STATE_EDITED);
    return;
  }
}

static void init_theme()
{
  /*Initialize the styles*/

  // numberedit
  // LV_PART_MAIN
  lv_style_init(&numberedit_style_main);
  lv_style_init(&numberedit_style_focused);
  lv_style_init(&numberedit_style_cursor);
  lv_style_init(&numberedit_style_cursor_edit);

  lv_style_set_border_width(&numberedit_style_main, 1);
  lv_style_set_border_color(&numberedit_style_main, makeLvColor(COLOR_THEME_SECONDARY2));
  lv_style_set_bg_color(&numberedit_style_main, makeLvColor(COLOR_THEME_PRIMARY2));
  lv_style_set_bg_opa(&numberedit_style_main, LV_OPA_COVER);
  lv_style_set_text_color(&numberedit_style_main, makeLvColor(COLOR_THEME_SECONDARY1));
  lv_style_set_pad_left(&numberedit_style_main, FIELD_PADDING_LEFT);
  lv_style_set_pad_top(&numberedit_style_main, FIELD_PADDING_TOP);

  // LV_STATE_FOCUSED
  lv_style_set_bg_color(&numberedit_style_focused, makeLvColor(COLOR_THEME_FOCUS));
  lv_style_set_text_color(&numberedit_style_focused, makeLvColor(COLOR_THEME_PRIMARY2));

  // Hide cursor when not editing
  lv_style_set_opa(&numberedit_style_cursor, LV_OPA_0);

  // Show Cursor in "Edit" mode
  lv_style_set_opa(&numberedit_style_cursor_edit, LV_OPA_COVER);
  lv_style_set_bg_opa(&numberedit_style_cursor_edit, LV_OPA_50);

  // textedit
  lv_style_init(&textedit_style_main);
  lv_style_init(&textedit_style_focused);
  lv_style_init(&textedit_style_cursor);
  lv_style_init(&textedit_style_cursor_edit);
  
  // textedit style main
  lv_style_set_border_width(&textedit_style_main, 1);
  lv_style_set_border_color(&textedit_style_main, makeLvColor(COLOR_THEME_SECONDARY2));
  lv_style_set_bg_color(&textedit_style_main, makeLvColor(COLOR_THEME_PRIMARY2));
  lv_style_set_bg_opa(&textedit_style_main, LV_OPA_COVER);
  lv_style_set_text_color(&textedit_style_main, makeLvColor(COLOR_THEME_SECONDARY1));
  lv_style_set_pad_left(&textedit_style_main, FIELD_PADDING_LEFT);
  lv_style_set_pad_top(&textedit_style_main, FIELD_PADDING_TOP);

  //textedit style focused
  lv_style_set_bg_color(&textedit_style_focused, makeLvColor(COLOR_THEME_FOCUS));
  lv_style_set_text_color(&textedit_style_focused, makeLvColor(COLOR_THEME_PRIMARY2));

  // hide cursor when not editing
  lv_style_set_opa(&textedit_style_cursor, LV_OPA_0);

  // Show Cursor in "Edit" mode
  lv_style_set_opa(&textedit_style_cursor_edit, LV_OPA_COVER);
  lv_style_set_bg_opa(&textedit_style_cursor_edit, LV_OPA_50);

  // Menus
  lv_style_init(&menu_line_style);
  lv_style_set_bg_opa(&menu_line_style, LV_OPA_100);
  lv_style_set_bg_color(&menu_line_style, makeLvColor(COLOR_THEME_PRIMARY2));

  lv_style_init(&menu_checked_style);
  lv_style_set_border_width(&menu_checked_style, 1);
  lv_style_set_border_color(&menu_checked_style, makeLvColor(COLOR_THEME_FOCUS));
  lv_style_set_text_color(&menu_checked_style, makeLvColor(COLOR_THEME_PRIMARY2));
  lv_style_set_bg_color(&menu_checked_style, makeLvColor(COLOR_THEME_FOCUS));

  /*Initialize the new theme from the current theme*/
  lv_theme_t *th_act = etx_lv_theme_init(
      NULL, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED),
      false, LV_FONT_DEFAULT);
  static lv_theme_t th_new;
  th_new = *th_act;

  /*Set the parent theme and the style apply callback for the new theme*/
  lv_theme_set_parent(&th_new, th_act);
  lv_theme_set_apply_cb(&th_new, theme_apply_cb);

  /*Assign the new theme to the current display*/
  lv_disp_set_theme(NULL, &th_new);
}

LvglWrapper::LvglWrapper()
{
  init_lvgl_drivers();
  init_theme();

  // Create main window and load that screen
  auto window = MainWindow::instance();
  window->setActiveScreen();
}

LvglWrapper* LvglWrapper::instance()
{
  if (!_instance) _instance = new LvglWrapper();
  return _instance;
}

void LvglWrapper::run()
{
#if defined(SIMU)
  tmr10ms_t tick = get_tmr10ms();
  lv_tick_inc((tick - lastTick) * 10);
  lastTick = tick;
#endif
  lv_timer_handler();
}

void LvglWrapper::pollInputs()
{
  lv_indev_t* indev = nullptr;
  while((indev = lv_indev_get_next(indev)) != nullptr) {
    lv_indev_read_timer_cb(indev->driver->read_timer);
  }
}
