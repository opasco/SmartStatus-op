#include "pebble.h"

#include "globals.h"

#define MAX(a, b) (((a) < (b)) ? (b) : (a))

#define DEBUG 0
#define STRING_LENGTH 255
#define NUM_WEATHER_IMAGES	8
#define SWAP_BOTTOM_LAYER_INTERVAL 15000
#define GPS_UPDATE_INTERVAL 60000
#define RECOVERY_ATTEMPT_INTERVAL 10000
#define DEFAULT_SONG_UPDATE_INTERVAL 5000

#define TIMER_COOKIE_WEATHER 1
#define TIMER_COOKIE_CALANDAR 2
#define TIMER_COOKIE_MUSIC 3
#define TIMER_COOKIE_LAYERSWAP 4
#define TIMER_COOKIE_NEXTDAYWEATHER 5
#define TIMER_COOKIE_GPS 6
#define TIMER_COOKIE_CONNECTIONRECOVER 7

typedef enum {MUSIC_LAYER, LOCATION_LAYER, NUM_LAYERS} AnimatedLayers;


AppMessageResult sm_message_out_get(DictionaryIterator **iter_out);
void reset_sequence_number();

void sendCommand(int key);
void sendCommandInt(int key, int param);
void rcv(DictionaryIterator *received, void *context);
void dropped(AppMessageResult reason, void *context);
void select_up_handler(ClickRecognizerRef recognizer, void *context);
void select_down_handler(ClickRecognizerRef recognizer, void *context);
void up_single_click_handler(ClickRecognizerRef recognizer, void *context);
void down_single_click_handler(ClickRecognizerRef recognizer, void *context);
void config_provider();
void battery_layer_update_callback(Layer *me, GContext* ctx);
void pebble_battery_layer_update_callback(Layer *me, GContext* ctx);
void handle_status_appear(Window *window);
void handle_status_disappear(Window *window);
void handle_minute_tick(struct tm* tick_time, TimeUnits units_changed);
void reset();	
void swap_bottom_layer();
	
Window *window;
PropertyAnimation *ani_out, *ani_in;

Layer *animated_layer[NUM_LAYERS], *weather_layer;
Layer *battery_layer, *battery_ind_layer, *calendar_layer;
Layer *pebble_battery_layer, *pebble_battery_ind_layer;

TextLayer *text_date_layer, *text_time_layer;

TextLayer *text_weather_cond_layer, *text_weather_temp_layer, *text_weather_tomorrow_temp_layer, *text_battery_layer;
TextLayer *calendar_date_layer, *calendar_text_layer, *text_status_layer;
TextLayer *music_artist_layer, *music_song_layer, *location_street_layer;
 
BitmapLayer *background_image, *weather_image, *weather_tomorrow_image;
BitmapLayer *battery_image_layer, *pebble_battery_image_layer;
BitmapLayer *phone_icon_layer, *pebble_icon_layer;

int32_t active_layer;
int32_t updateGPSInterval = GPS_UPDATE_INTERVAL;
bool connected = 0;
int8_t inTimeOut = 0;
bool inGPSUpdate = 0;
bool sending = 0;
int8_t current_app = -1;

char string_buffer[STRING_LENGTH], location_street_str[STRING_LENGTH], appointment_time[15];
char weather_cond_str[STRING_LENGTH], weather_tomorrow_temp_str[5], weather_temp_str[5];
int32_t weather_img, weather_tomorrow_img, batteryPercent, pebble_batteryPercent;

char calendar_date_str[STRING_LENGTH], calendar_text_str[STRING_LENGTH];
char music_artist_str[STRING_LENGTH], music_title_str[STRING_LENGTH];


GBitmap *battery_image, *pebble_battery_image, *phone_icon, *pebble_icon;
GBitmap *weather_status_small_imgs[NUM_WEATHER_IMAGES];

AppTimer *timerUpdateCalendar = NULL;
AppTimer *timerUpdateWeather = NULL;
AppTimer *timerUpdateMusic = NULL;
AppTimer *timerSwapBottomLayer = NULL;
AppTimer *timerUpdateWeatherForecast = NULL;
AppTimer *timerUpdateGps = NULL;
AppTimer *timerRecoveryAttempt = NULL;

const int WEATHER_SMALL_IMG_IDS[] = {
  RESOURCE_ID_IMAGE_SUN_SMALL,
  RESOURCE_ID_IMAGE_RAIN_SMALL,
  RESOURCE_ID_IMAGE_CLOUD_SMALL,
  RESOURCE_ID_IMAGE_SUN_CLOUD_SMALL,
  RESOURCE_ID_IMAGE_FOG_SMALL,
  RESOURCE_ID_IMAGE_WIND_SMALL,
  RESOURCE_ID_IMAGE_SNOW_SMALL,
  RESOURCE_ID_IMAGE_THUNDER_SMALL
};



uint32_t s_sequence_number = 0xFFFFFFFE;

/* Convert letter to digit */
int letter2digit(char letter) {
	if((letter >= 48) && (letter <=57)) {
		return letter - 48;
	}
	
	return -1;
}

/* Convert string to number */
int string2number(char *string) {
	int32_t result = 0;
	int32_t offset;
	int32_t digit = -1;
	int32_t unit = 1;
	int8_t letter;

	offset = strlen(string) - 1;

	for(unit = 1; offset >= 0; unit = unit * 10) {
		letter = string[offset];
		digit = letter2digit(letter);
		if(digit == -1) return -1;
		result = result + (unit * digit);
		offset--;
	}
	
	return result;
}

/* Convert time string ("HH:MM") to number of minutes */
int timestr2minutes(char *timestr) {
	static char hourStr[3], minStr[3];
	int32_t hour, min;
	int8_t hDigits = 2;

	if(timestr[1] == ':') hDigits = 1;
	
	strncpy(hourStr, timestr, hDigits);
	strncpy(minStr, timestr+hDigits+1, 2);
	
	hour = string2number(hourStr);
	if(hour == -1) return -1;
	
	min = string2number(minStr);
	if(min == -1) return -1;
	
	return min + (hour * 60);
}

static void apptDisplay() {
	int32_t apptInMinutes, timeInMinutes;
	static char date_time_for_appt[] = "....................";
	time_t now;
	struct tm *t;
	
	now = time(NULL);
	t = localtime(&now);
	
	strftime(date_time_for_appt, sizeof(date_time_for_appt), "%m/%d", t);
	
	if(strncmp(date_time_for_appt, appointment_time, 5) != 0) {
		text_layer_set_text(calendar_date_layer, appointment_time);
		layer_set_hidden(calendar_layer, 0);
		return;
	}

	/* Manage appoitment notification */
	apptInMinutes = timestr2minutes(appointment_time + 6);
	if(apptInMinutes >= 0) {
		timeInMinutes = (t->tm_hour * 60) + t->tm_min;
		if(apptInMinutes < timeInMinutes) {
			if(((timeInMinutes - apptInMinutes) / 60) > 0) {
				snprintf(date_time_for_appt, 20, "In %dh %dm", 
						 (int)((timeInMinutes - apptInMinutes) / 60),
						 (int)((timeInMinutes - apptInMinutes) % 60));
			} else {
				snprintf(date_time_for_appt, 20, "%d min in", (int)(timeInMinutes - apptInMinutes));
			}
			text_layer_set_text(calendar_date_layer, date_time_for_appt); 	
			layer_set_hidden(calendar_layer, 0);  	
		}
		if(apptInMinutes > timeInMinutes) {
			if(((apptInMinutes - timeInMinutes) / 60) > 0) {
				snprintf(date_time_for_appt, 20, "In %dh %dm", 
						 (int)((apptInMinutes - timeInMinutes) / 60),
						 (int)((apptInMinutes - timeInMinutes) % 60));
			} else {
				snprintf(date_time_for_appt, 20, "In %d min", (int)(apptInMinutes - timeInMinutes));
			}
			text_layer_set_text(calendar_date_layer, date_time_for_appt); 	
			layer_set_hidden(calendar_layer, 0);  	
		}
		if(apptInMinutes == timeInMinutes) {
			text_layer_set_text(calendar_date_layer, "Now!"); 	
			layer_set_hidden(calendar_layer, 0);  	
			vibes_double_pulse();
		}
		if((apptInMinutes >= timeInMinutes) && ((apptInMinutes - timeInMinutes) == 15)) {
			vibes_short_pulse();
		}
	}
	
	layer_set_hidden(calendar_layer, 0);
}
 
AppMessageResult sm_message_out_get(DictionaryIterator **iter_out) {
    AppMessageResult result = app_message_outbox_begin(iter_out);
    if(result != APP_MSG_OK) return result;
    dict_write_int32(*iter_out, SM_SEQUENCE_NUMBER_KEY, ++s_sequence_number);
    if(s_sequence_number == 0xFFFFFFFF) {
        s_sequence_number = 1;
    }
	if(DEBUG)
		text_layer_set_text(text_status_layer, "Send.");
    return APP_MSG_OK;
}

void reset_sequence_number() {
	if(!bluetooth_connection_service_peek()) return;
	
    DictionaryIterator *iter = NULL;
    app_message_outbox_begin(&iter);
    if(!iter) return;
    dict_write_int32(iter, SM_SEQUENCE_NUMBER_KEY, 0xFFFFFFFF);
    app_message_outbox_send();
}


void sendCommand(int key) {
	if(!bluetooth_connection_service_peek()) return;

	if(sending == 1) return;

	DictionaryIterator* iterout = NULL;
	sm_message_out_get(&iterout);
    if(!iterout) return;
	
	if(dict_write_int8(iterout, key, -1) != DICT_OK) return;
	sending = 1;
	app_message_outbox_send();
}


void sendCommandInt(int key, int param) {
	if(sending == 1) return;

	DictionaryIterator* iterout = NULL;
	sm_message_out_get(&iterout);
    if(!iterout) return;
	
	if(dict_write_int8(iterout, key, param) != DICT_OK) return;
	sending = 1;
	app_message_outbox_send();
}

// Timer callbacks
void timer_cbk_weather() {
	if(timerUpdateWeather) {
		app_timer_cancel(timerUpdateWeather);
		timerUpdateWeather = NULL;
	}

	if(current_app != STATUS_SCREEN_APP)
		sendCommandInt(SM_SCREEN_ENTER_KEY, WEATHER_APP);
	else
		sendCommand(SM_STATUS_UPD_WEATHER_KEY);	
}
		
void timer_cbk_calandar() {
	if(timerUpdateCalendar) {
		app_timer_cancel(timerUpdateCalendar);
		timerUpdateCalendar = NULL;
	}

	if(current_app != STATUS_SCREEN_APP)
		sendCommandInt(SM_SCREEN_ENTER_KEY, STATUS_SCREEN_APP);
	else
		sendCommand(SM_STATUS_UPD_CAL_KEY);	
}

void timer_cbk_music() {
	if(timerUpdateMusic) {
		app_timer_cancel(timerUpdateMusic);
		timerUpdateMusic = NULL;
	}
	
	if(current_app != STATUS_SCREEN_APP)
		sendCommandInt(SM_SCREEN_ENTER_KEY, STATUS_SCREEN_APP);
	else
		sendCommand(SM_SONG_LENGTH_KEY);	
}

void timer_cbk_layerswap() {
	swap_bottom_layer();	

	if(timerSwapBottomLayer) {
		app_timer_cancel(timerSwapBottomLayer);
		timerSwapBottomLayer = NULL;
	}
	timerSwapBottomLayer = app_timer_register(SWAP_BOTTOM_LAYER_INTERVAL, timer_cbk_layerswap, NULL);
}
	
void timer_cbk_nextdayweather() {
	if(timerUpdateWeatherForecast) {
		app_timer_cancel(timerUpdateWeatherForecast);
		timerUpdateWeatherForecast = NULL;
	}

	if(current_app != WEATHER_APP)
		sendCommandInt(SM_SCREEN_ENTER_KEY, WEATHER_APP);
	else
		sendCommand(SM_STATUS_UPD_WEATHER_KEY);	
}
		
void timer_cbk_gps() {
	if(current_app != GPS_APP)
		sendCommandInt(SM_SCREEN_ENTER_KEY, GPS_APP);
		
	if(timerUpdateGps) {
		app_timer_cancel(timerUpdateGps);
		timerUpdateGps = NULL;
	}
	timerUpdateGps = app_timer_register(updateGPSInterval, timer_cbk_gps, NULL);
}
	
void timer_cbk_connectionrecover() {
	reset_sequence_number();
	sendCommandInt(SM_SCREEN_ENTER_KEY, STATUS_SCREEN_APP);
}

void rcv(DictionaryIterator *received, void *context) {
	// Got a message callback
	Tuple *t;
	int8_t interval;
	
	connected = 1;

	t = dict_find(received, SM_COUNT_BATTERY_KEY); 
	
	if (t!=NULL) {
		batteryPercent = t->value->uint8;
		layer_mark_dirty(battery_ind_layer);
	}

	t=dict_find(received, SM_WEATHER_TEMP_KEY); 
	if (t!=NULL) {
		memcpy(weather_temp_str, t->value->cstring, 
			   MAX(sizeof(weather_temp_str),strlen(t->value->cstring)));
        weather_temp_str[strlen(t->value->cstring)] = '\0';
		text_layer_set_text(text_weather_temp_layer, weather_temp_str); 
	}

	t=dict_find(received, SM_WEATHER_ICON_KEY); 
	if (t!=NULL) {
		bitmap_layer_set_bitmap(weather_image, weather_status_small_imgs[t->value->uint8]);	  	
	}

	t=dict_find(received, SM_WEATHER_ICON1_KEY); 
	if (t!=NULL) {
		bitmap_layer_set_bitmap(weather_tomorrow_image, weather_status_small_imgs[t->value->uint8]);	  	
	}
	
	t=dict_find(received, SM_WEATHER_DAY1_KEY); 
	if (t!=NULL) {
		memcpy(weather_tomorrow_temp_str, t->value->cstring + 6,
			   MAX(sizeof(weather_tomorrow_temp_str), strlen(t->value->cstring)));
        weather_tomorrow_temp_str[strlen(t->value->cstring)] = '\0';
		text_layer_set_text(text_weather_tomorrow_temp_layer, weather_tomorrow_temp_str); 	
	}

	t=dict_find(received, SM_UPDATE_INTERVAL_KEY); 
	if (t!=NULL) {
		if(inGPSUpdate == 1) {
			updateGPSInterval = t->value->int32 * 1000;
			inGPSUpdate = 0;
		}
	}

	t=dict_find(received, SM_GPS_1_KEY); 
	if (t!=NULL) {
		memcpy(location_street_str, t->value->cstring,
			   MAX(sizeof(location_street_str), strlen(t->value->cstring)));
        location_street_str[strlen(t->value->cstring)] = '\0';
		text_layer_set_text(location_street_layer, location_street_str);
	}

	t=dict_find(received, SM_STATUS_CAL_TIME_KEY); 
	if (t!=NULL) {
		memcpy(calendar_date_str, t->value->cstring,
			   MAX(sizeof(calendar_date_str), strlen(t->value->cstring)));
        calendar_date_str[strlen(t->value->cstring)] = '\0';
		//text_layer_set_text(calendar_date_layer, calendar_date_str); 	
		strncpy(appointment_time, calendar_date_str, 11);
	}

	t=dict_find(received, SM_STATUS_CAL_TEXT_KEY); 
	if (t!=NULL) {
		memcpy(calendar_text_str, t->value->cstring,
			   MAX(sizeof(calendar_text_str), strlen(t->value->cstring)));
        calendar_text_str[strlen(t->value->cstring)] = '\0';
		text_layer_set_text(calendar_text_layer, calendar_text_str); 	
		
		if(strlen(calendar_text_str) <= 15)
			text_layer_set_font(calendar_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
		else
			if(strlen(calendar_text_str) <= 18)
				text_layer_set_font(calendar_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
			else 
				text_layer_set_font(calendar_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
	}

	t=dict_find(received, SM_STATUS_MUS_ARTIST_KEY); 
	if (t!=NULL) {
		memcpy(music_artist_str, t->value->cstring,
			   MAX(sizeof(music_artist_str), strlen(t->value->cstring)));
        music_artist_str[strlen(t->value->cstring)] = '\0';
		text_layer_set_text(music_artist_layer, music_artist_str); 	
	}

	t=dict_find(received, SM_STATUS_MUS_TITLE_KEY); 
	if (t!=NULL) {
		memcpy(music_title_str, t->value->cstring,
			   MAX(sizeof(music_title_str), strlen(t->value->cstring)));
        music_title_str[strlen(t->value->cstring)] = '\0';
		text_layer_set_text(music_song_layer, music_title_str); 	
	}

	t=dict_find(received, SM_STATUS_UPD_WEATHER_KEY); 
	if (t!=NULL) {
		interval = t->value->int32 * 1000;

		if(timerUpdateWeather) {
			app_timer_cancel(timerUpdateWeather);
			timerUpdateWeather = NULL;
		}
		timerUpdateWeather = app_timer_register(interval, timer_cbk_weather, NULL);
	}

	t=dict_find(received, SM_STATUS_UPD_CAL_KEY); 
	if (t!=NULL) {
		interval = t->value->int32 * 1000;

		if(timerUpdateCalendar) {
			app_timer_cancel(timerUpdateCalendar);
			timerUpdateCalendar = NULL;
		}
		timerUpdateCalendar = app_timer_register(interval, timer_cbk_calandar, NULL);
	}

	t=dict_find(received, SM_SONG_LENGTH_KEY); 
	if (t!=NULL) {
		interval = t->value->int32 * 1000;

		if(timerUpdateMusic) {
			app_timer_cancel(timerUpdateMusic);
			timerUpdateMusic = NULL;
		}
		timerUpdateMusic = app_timer_register(interval, timer_cbk_music, NULL);
	}
	
	if(!DEBUG)
		text_layer_set_text(text_status_layer, "");

}

void dropped(AppMessageResult reason, void *context){
	// DO SOMETHING WITH THE DROPPED REASON / DISPLAY AN ERROR / RESEND 
	text_layer_set_text(text_status_layer, "Drop.");
	
	if(reason == APP_MSG_BUSY) {
		text_layer_set_text(text_status_layer, ">Busy");
	}
	
	if(reason == APP_MSG_BUFFER_OVERFLOW) {
		text_layer_set_text(text_status_layer, "Over.");
	}
	
	connected = 0;

	if(timerRecoveryAttempt) {
		app_timer_cancel(timerRecoveryAttempt);
		timerRecoveryAttempt = NULL;
	}
	timerRecoveryAttempt = app_timer_register(RECOVERY_ATTEMPT_INTERVAL, timer_cbk_connectionrecover, NULL);
}

void sent_ok(DictionaryIterator *sent, void *context) {
	Tuple *t;
	
	if(timerRecoveryAttempt) {
		app_timer_cancel(timerRecoveryAttempt);
		timerRecoveryAttempt = NULL;
	}

	t = dict_find(sent, SM_SCREEN_ENTER_KEY);
	if(t) current_app = t->value->int8;

	sending = 0;
	if(DEBUG)
		text_layer_set_text(text_status_layer, "Ok");
	
	connected = 1;
	inTimeOut = 0;
}

void send_failed(DictionaryIterator *failed, AppMessageResult reason, void *context) {
	sending = 0;
	text_layer_set_text(text_status_layer, "Err.");
	
	if(reason == APP_MSG_NOT_CONNECTED) {
		text_layer_set_text(text_status_layer, "Disc.");
		if(connected == 1) {
			vibes_double_pulse();
		}
	}
	
	if(reason == APP_MSG_SEND_TIMEOUT) {
		text_layer_set_text(text_status_layer, "T.Out");
 		if(inTimeOut == 0) {
			inTimeOut = 1;
		} else if(inTimeOut == 1) {
			vibes_double_pulse();
			inTimeOut = 2;
		}
	}
	
	if(reason == APP_MSG_BUSY) {
		text_layer_set_text(text_status_layer, "<Busy");
	}
	
	if(reason == APP_MSG_SEND_REJECTED) {
		text_layer_set_text(text_status_layer, "Nack");
	}
	
	connected = 0;

	if(timerRecoveryAttempt) {
		app_timer_cancel(timerRecoveryAttempt);
		timerRecoveryAttempt = NULL;
	}
	timerRecoveryAttempt = app_timer_register(RECOVERY_ATTEMPT_INTERVAL, timer_cbk_connectionrecover, NULL);
}


void select_single_click_handler(ClickRecognizerRef recognizer, void *context) {
	sendCommand(SM_PLAYPAUSE_KEY);
}

void select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
	sendCommand(SM_FIND_MY_PHONE_KEY);
}

void select_up_handler(ClickRecognizerRef recognizer, void *context) {
}


void select_down_handler(ClickRecognizerRef recognizer, void *context) {
}


void up_single_click_handler(ClickRecognizerRef recognizer, void *context) {
	sendCommand(SM_VOLUME_UP_KEY);
}

void down_single_click_handler(ClickRecognizerRef recognizer, void *context) {
	sendCommand(SM_VOLUME_DOWN_KEY);
}

void swap_bottom_layer() {
	return;
	//on a press of the bottom button, scroll in the next layer

	ani_out = property_animation_create_layer_frame(animated_layer[active_layer], &GRect(30, 72, 75, 50), &GRect(-75, 72, 75, 50));
	animation_schedule(&(ani_out->animation));

	active_layer = (active_layer + 1) % (NUM_LAYERS);

	ani_in = property_animation_create_layer_frame(animated_layer[active_layer], &GRect(144, 72, 75, 50), &GRect(30, 72, 75, 50));
	animation_schedule(&(ani_in->animation));
}


void config_provider() {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_single_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 0, select_long_click_handler, NULL);
  window_single_click_subscribe(BUTTON_ID_UP, up_single_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_single_click_handler);
}


void battery_layer_update_callback(Layer *me, GContext* ctx) {
	//draw the remaining battery percentage
	graphics_context_set_stroke_color(ctx, GColorBlack);
	graphics_context_set_fill_color(ctx, GColorWhite);

	graphics_fill_rect(ctx, GRect(2+16-(int)((batteryPercent/100.0)*16.0), 2, (int)((batteryPercent/100.0)*16.0), 8), 0, GCornerNone);
	
	if(batteryPercent < 20) vibes_double_pulse();
}

void pebble_battery_layer_update_callback(Layer *me, GContext* ctx) {
	//draw the remaining battery percentage
	graphics_context_set_stroke_color(ctx, GColorBlack);
	graphics_context_set_fill_color(ctx, GColorWhite);

	graphics_fill_rect(ctx, GRect(2+16-(int)((pebble_batteryPercent/100.0)*16.0), 2, (int)((pebble_batteryPercent/100.0)*16.0), 8), 0, GCornerNone);
	
	if(pebble_batteryPercent < 20) vibes_double_pulse();
}

void window_load(Window *this) {
	Layer *window_layer = window_get_root_layer(this);
	GRect bounds = layer_get_bounds(window_layer);

	//init weather images
	for (int8_t i=0; i<NUM_WEATHER_IMAGES; i++) {
		weather_status_small_imgs[i] = gbitmap_create_with_resource(WEATHER_SMALL_IMG_IDS[i]);
	}
	
	// init battery layer
	battery_layer = layer_create(GRect(95, 45, 49, 45));
	layer_add_child(window_layer, battery_layer);

	battery_image = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_BATTERY);
	phone_icon = gbitmap_create_with_resource(RESOURCE_ID_PHONE_ICON);
	pebble_icon = gbitmap_create_with_resource(RESOURCE_ID_PEBBLE_ICON);

	battery_image_layer = bitmap_layer_create(GRect(12, 8, 23, 14));
	layer_add_child(battery_layer, bitmap_layer_get_layer(battery_image_layer));
	bitmap_layer_set_bitmap(battery_image_layer, battery_image);
	
	phone_icon_layer = bitmap_layer_create(GRect(-5, 5, 20, 20));
	layer_add_child(battery_layer, bitmap_layer_get_layer(phone_icon_layer));
	bitmap_layer_set_bitmap(phone_icon_layer, phone_icon);

	battery_ind_layer = layer_create(GRect(14, 9, 19, 11));
	layer_set_update_proc(battery_ind_layer, battery_layer_update_callback);
	layer_add_child(battery_layer, battery_ind_layer);

	batteryPercent = 100;
	layer_mark_dirty(battery_ind_layer);

	// init Pebble battery layer
	pebble_battery_layer = layer_create(GRect(-5, 45, 49, 45));
	layer_add_child(window_layer, pebble_battery_layer);

	pebble_battery_image = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_BATTERY);

	pebble_battery_image_layer = bitmap_layer_create(GRect(12, 8, 23, 14));
	layer_add_child(pebble_battery_layer, bitmap_layer_get_layer(pebble_battery_image_layer));
	bitmap_layer_set_bitmap(pebble_battery_image_layer, pebble_battery_image);
	
	pebble_icon_layer = bitmap_layer_create(GRect(31, 5, 20, 20));
	layer_add_child(pebble_battery_layer, bitmap_layer_get_layer(pebble_icon_layer));
	bitmap_layer_set_bitmap(pebble_icon_layer, pebble_icon);

	pebble_battery_ind_layer = layer_create(GRect(14, 9, 19, 11));
	layer_set_update_proc(pebble_battery_ind_layer, pebble_battery_layer_update_callback);
	layer_add_child(pebble_battery_layer, pebble_battery_ind_layer);
	
	BatteryChargeState pb_bat = battery_state_service_peek();
	pebble_batteryPercent = pb_bat.charge_percent;
	layer_mark_dirty(pebble_battery_layer);

	//init weather layer and add weather image, weather condition, temperature
	weather_layer = layer_create(GRect(0, 70, 144, 45));
	layer_add_child(window_layer, weather_layer);


	weather_img = 0;

	weather_image = bitmap_layer_create(GRect(5, 4, 20, 20)); // GRect(52, 2, 40, 40)
	layer_add_child(weather_layer, bitmap_layer_get_layer(weather_image));
	bitmap_layer_set_bitmap(weather_image, weather_status_small_imgs[0]);

	weather_tomorrow_img = 0;

	weather_tomorrow_image = bitmap_layer_create(GRect(112, 4, 20, 20)); // GRect(52, 2, 40, 40)
	layer_add_child(weather_layer, bitmap_layer_get_layer(weather_tomorrow_image));
	bitmap_layer_set_bitmap(weather_tomorrow_image, weather_status_small_imgs[0]);

	text_weather_tomorrow_temp_layer = text_layer_create(GRect(105, 23, 31, 20)); // GRect(5, 2, 47, 40)
	text_layer_set_text_alignment(text_weather_tomorrow_temp_layer, GTextAlignmentCenter);
	text_layer_set_text_color(text_weather_tomorrow_temp_layer, GColorWhite);
	text_layer_set_background_color(text_weather_tomorrow_temp_layer, GColorClear);
	text_layer_set_font(text_weather_tomorrow_temp_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
	layer_add_child(weather_layer, text_layer_get_layer(text_weather_tomorrow_temp_layer));
	text_layer_set_text(text_weather_tomorrow_temp_layer, "../.."); 	
	
	text_weather_temp_layer = text_layer_create(GRect(5, 23, 25, 20)); // GRect(98, 4, 47, 40)
	text_layer_set_text_alignment(text_weather_temp_layer, GTextAlignmentCenter);
	text_layer_set_text_color(text_weather_temp_layer, GColorWhite);
	text_layer_set_background_color(text_weather_temp_layer, GColorClear);
	text_layer_set_font(text_weather_temp_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
	layer_add_child(weather_layer, text_layer_get_layer(text_weather_temp_layer));
	text_layer_set_text(text_weather_temp_layer, "-°"); 	
	
	//init layers for time and date and status
	text_date_layer = text_layer_create(bounds);
	text_layer_set_text_alignment(text_date_layer, GTextAlignmentLeft);
	text_layer_set_text_color(text_date_layer, GColorWhite);
	text_layer_set_background_color(text_date_layer, GColorClear);
	layer_set_frame(text_layer_get_layer(text_date_layer), GRect(47, 48, 50, 30));
	//text_layer_set_font(text_date_layer, fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_ROBOTO_CONDENSED_21)));
	text_layer_set_font(text_date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
	layer_add_child(window_layer, text_layer_get_layer(text_date_layer));


	text_time_layer = text_layer_create(bounds);
	text_layer_set_text_alignment(text_time_layer, GTextAlignmentCenter);
	text_layer_set_text_color(text_time_layer, GColorWhite);
	text_layer_set_background_color(text_time_layer, GColorClear);
	layer_set_frame(text_layer_get_layer(text_time_layer), GRect(0, -5, 144, 50));
	text_layer_set_font(text_time_layer, fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_ROBOTO_BOLD_SUBSET_49)));
	layer_add_child(window_layer, text_layer_get_layer(text_time_layer));

	text_status_layer = text_layer_create(GRect(6, 110, 45, 20));
	text_layer_set_text_alignment(text_status_layer, GTextAlignmentLeft);
	text_layer_set_text_color(text_status_layer, GColorWhite);
	text_layer_set_background_color(text_status_layer, GColorClear);
	text_layer_set_font(text_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
	layer_add_child(window_layer, text_layer_get_layer(text_status_layer));
	text_layer_set_text(text_status_layer, "Init.");


	//init calendar layer
	calendar_layer = layer_create(GRect(0, 124, 144, 45));
	layer_add_child(window_layer, calendar_layer);
	
	calendar_date_layer = text_layer_create(GRect(6, 0, 132, 21));
	text_layer_set_text_alignment(calendar_date_layer, GTextAlignmentLeft);
	text_layer_set_text_color(calendar_date_layer, GColorWhite);
	text_layer_set_background_color(calendar_date_layer, GColorClear);
	text_layer_set_font(calendar_date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
	layer_add_child(calendar_layer, text_layer_get_layer(calendar_date_layer));
	text_layer_set_text(calendar_date_layer, "No Upcoming"); 	


	calendar_text_layer = text_layer_create(GRect(6, 15, 132, 29));
	text_layer_set_text_alignment(calendar_text_layer, GTextAlignmentLeft);
	text_layer_set_text_color(calendar_text_layer, GColorWhite);
	text_layer_set_background_color(calendar_text_layer, GColorClear);
	text_layer_set_font(calendar_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
	layer_add_child(calendar_layer, text_layer_get_layer(calendar_text_layer));
	text_layer_set_text(calendar_text_layer, "Appointment");
	
	
	
	//init music layer
	animated_layer[MUSIC_LAYER] = layer_create(GRect(144, 72, 75, 50));
	layer_add_child(window_layer, animated_layer[MUSIC_LAYER]);
	
	music_artist_layer = text_layer_create(GRect(0, 0, 75, 24));
	text_layer_set_text_alignment(music_artist_layer, GTextAlignmentCenter);
	text_layer_set_text_color(music_artist_layer, GColorWhite);
	text_layer_set_background_color(music_artist_layer, GColorClear);
	text_layer_set_font(music_artist_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
	layer_add_child(animated_layer[MUSIC_LAYER], text_layer_get_layer(music_artist_layer));
	text_layer_set_text(music_artist_layer, "No Artist"); 	


	music_song_layer = text_layer_create(GRect(0, 25, 75, 25));
	text_layer_set_text_alignment(music_song_layer, GTextAlignmentCenter);
	text_layer_set_text_color(music_song_layer, GColorWhite);
	text_layer_set_background_color(music_song_layer, GColorClear);
	text_layer_set_font(music_song_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
	layer_add_child(animated_layer[MUSIC_LAYER], text_layer_get_layer(music_song_layer));
	text_layer_set_text(music_song_layer, "No Title");
	
	
	//init location layer
	animated_layer[LOCATION_LAYER] = layer_create(GRect(30, 72, 75, 50));
	layer_add_child(window_layer, animated_layer[LOCATION_LAYER]);
	
	location_street_layer = text_layer_create(GRect(0, 0, 75, 47));
	text_layer_set_text_alignment(location_street_layer, GTextAlignmentCenter);
	text_layer_set_text_color(location_street_layer, GColorWhite);
	text_layer_set_background_color(location_street_layer, GColorClear);
	text_layer_set_font(location_street_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
	layer_add_child(animated_layer[LOCATION_LAYER], text_layer_get_layer(location_street_layer));
	text_layer_set_text(location_street_layer, "Location not updated"); 	

	active_layer = LOCATION_LAYER;
	
	timerUpdateWeatherForecast = app_timer_register(5000, timer_cbk_nextdayweather, NULL);

	if(DEBUG)
		text_layer_set_text(text_status_layer, "Hello");
	
	sendCommandInt(SM_SCREEN_ENTER_KEY, STATUS_SCREEN_APP);

	// Start UI timers	
	timerSwapBottomLayer = app_timer_register(SWAP_BOTTOM_LAYER_INTERVAL, timer_cbk_layerswap, NULL);
	timerUpdateGps = app_timer_register(updateGPSInterval, timer_cbk_gps, NULL);
	timerUpdateMusic = app_timer_register(DEFAULT_SONG_UPDATE_INTERVAL, timer_cbk_music, NULL);
}

void pebble_battery_update(BatteryChargeState pb_bat) {
	pebble_batteryPercent = pb_bat.charge_percent;
	layer_mark_dirty(pebble_battery_layer);
}

void bluetooth_connection_handler(bool btConnected) {
	if(btConnected) {
		sendCommandInt(SM_SCREEN_ENTER_KEY, STATUS_SCREEN_APP);
		
		if(!timerSwapBottomLayer)
			timerSwapBottomLayer = app_timer_register(SWAP_BOTTOM_LAYER_INTERVAL, timer_cbk_layerswap, NULL);
		if(!timerUpdateGps)
			timerUpdateGps = app_timer_register(updateGPSInterval, timer_cbk_gps, NULL);
		if(!timerUpdateMusic)
			timerUpdateMusic = app_timer_register(DEFAULT_SONG_UPDATE_INTERVAL, timer_cbk_music, NULL);

	} else {
		// Cancel all running timers
		if(timerUpdateCalendar) {
			app_timer_cancel(timerUpdateCalendar);
			timerUpdateCalendar = NULL;
		}
		if(timerUpdateMusic) {
			app_timer_cancel(timerUpdateMusic);
			timerUpdateMusic = NULL;
		}
		if(timerUpdateWeather) {
			app_timer_cancel(timerUpdateWeather);
			timerUpdateWeather = NULL;
		}
		if(timerSwapBottomLayer) {
			app_timer_cancel(timerSwapBottomLayer);
			timerSwapBottomLayer = NULL;
		}
		if(timerUpdateGps) {
			app_timer_cancel(timerUpdateGps);
			timerUpdateGps = NULL;
		}
		if(timerRecoveryAttempt) {
			app_timer_cancel(timerRecoveryAttempt);
			timerRecoveryAttempt = NULL;
		}
		if(timerUpdateWeatherForecast) {
			app_timer_cancel(timerUpdateWeatherForecast);
			timerUpdateWeatherForecast = NULL;
		}
	}
}

void handle_minute_tick(struct tm* tick_time, TimeUnits units_changed) {
/* Display the time */
  	static char time_text[] = "00:00";
  	static char date_text[] = "Xxxxxxxxx 00";
	
  	strftime(date_text, sizeof(date_text), "%b %e", tick_time);
  	text_layer_set_text(text_date_layer, date_text);


	if (clock_is_24h_style()) {
		strftime(time_text, sizeof(time_text), "%R", tick_time);
	} else {
		strftime(time_text, sizeof(time_text), "%I:%M", tick_time);
	}

  	if (!clock_is_24h_style() && (time_text[0] == '0')) {
    	memmove(time_text, &time_text[1], sizeof(time_text) - 1);
	}

  	text_layer_set_text(text_time_layer, time_text);
	
	apptDisplay();
}

void window_unload(Window *this) {
	if(DEBUG)
		text_layer_set_text(text_status_layer, "Bye");
	
	// Notify iPhone App
	sendCommandInt(SM_SCREEN_EXIT_KEY, STATUS_SCREEN_APP);
	
	// Cancel all running timers
	if(timerUpdateCalendar) {
		app_timer_cancel(timerUpdateCalendar);
		timerUpdateCalendar = NULL;
	}
	if(timerUpdateMusic) {
		app_timer_cancel(timerUpdateMusic);
		timerUpdateMusic = NULL;
	}
	if(timerUpdateWeather) {
		app_timer_cancel(timerUpdateWeather);
		timerUpdateWeather = NULL;
	}
	if(timerSwapBottomLayer) {
		app_timer_cancel(timerSwapBottomLayer);
		timerSwapBottomLayer = NULL;
	}
	if(timerUpdateGps) {
		app_timer_cancel(timerUpdateGps);
		timerUpdateGps = NULL;
	}
	if(timerRecoveryAttempt) {
		app_timer_cancel(timerRecoveryAttempt);
		timerRecoveryAttempt = NULL;
	}
	if(timerUpdateWeatherForecast) {
		app_timer_cancel(timerUpdateWeatherForecast);
		timerUpdateWeatherForecast = NULL;
	}
	
	
	// Clean up UI elements
	bitmap_layer_destroy(battery_image_layer);
	bitmap_layer_destroy(weather_image);
	bitmap_layer_destroy(weather_tomorrow_image);
	text_layer_destroy(text_weather_tomorrow_temp_layer);
	text_layer_destroy(text_weather_temp_layer);
	text_layer_destroy(text_date_layer);
	text_layer_destroy(text_time_layer);
	text_layer_destroy(text_status_layer);
	text_layer_destroy(calendar_date_layer);
	text_layer_destroy(calendar_text_layer);
	layer_destroy(calendar_layer);
	text_layer_destroy(music_artist_layer);
	text_layer_destroy(music_song_layer);
	text_layer_destroy(location_street_layer);
	layer_destroy(battery_layer);
	layer_destroy(animated_layer[MUSIC_LAYER]);
	layer_destroy(battery_ind_layer);
	layer_destroy(weather_layer);
	layer_destroy(animated_layer[LOCATION_LAYER]);

	// Release resources
	for (int8_t i=0; i<NUM_WEATHER_IMAGES; i++) {
		gbitmap_destroy(weather_status_small_imgs[i]);
	}
	gbitmap_destroy(battery_image);
	gbitmap_destroy(phone_icon);
	gbitmap_destroy(pebble_icon);
}

// App startup
static void do_init(void) {
	// Subscribe to required services
	tick_timer_service_subscribe(MINUTE_UNIT, &handle_minute_tick);
	battery_state_service_subscribe(pebble_battery_update);
	bluetooth_connection_service_subscribe(bluetooth_connection_handler);

	// Create app's base window
	window = window_create();
	window_set_window_handlers(window, (WindowHandlers) {
		.load = window_load,
		.unload = window_unload,
	});
	window_set_click_config_provider(window, (ClickConfigProvider) config_provider);

	// Initialize messaging
	app_message_register_inbox_received(rcv);
	app_message_register_inbox_dropped(dropped);
	app_message_register_outbox_sent(sent_ok);
	app_message_register_outbox_failed(send_failed);
	const uint32_t inbound_size = app_message_inbox_size_maximum();
	const uint32_t outbound_size = app_message_outbox_size_maximum();
	app_message_open(inbound_size, outbound_size);

	// Push the main window onto the stack
	const bool animated = true;
	window_set_fullscreen(window, true);
	window_stack_push(window, animated);
	window_set_background_color(window, GColorBlack);

	// Init global variables
	appointment_time[0] = '\0';
}

// Release resources
static void do_deinit(void) {
	// Unsubscribe services
	tick_timer_service_unsubscribe();
	battery_state_service_unsubscribe();
	bluetooth_connection_service_unsubscribe();
	
	// Deregister messaging callbacks
	app_message_deregister_callbacks();

	// Release windows
	window_destroy(window);
}

// The main event/run loop for our app
int main(void) {
  do_init();
  app_event_loop();
  do_deinit();
}