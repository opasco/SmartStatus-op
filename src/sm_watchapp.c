#include "pebble.h"

#include "globals.h"

#define DEBUG 0

#define MAX(a, b) (((a) < (b)) ? (b) : (a))
#define MIN(a, b) (((a) > (b)) ? (b) : (a))

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


static AppMessageResult sm_message_out_get(DictionaryIterator **iter_out);
static void reset_sequence_number();

static void sendCommand(int key);
static void sendCommandInt(int key, int param);
static void rcv(DictionaryIterator *received, void *context);
static void dropped(AppMessageResult reason, void *context);
static void select_up_handler(ClickRecognizerRef recognizer, void *context);
static void select_down_handler(ClickRecognizerRef recognizer, void *context);
static void up_single_click_handler(ClickRecognizerRef recognizer, void *context);
static void down_single_click_handler(ClickRecognizerRef recognizer, void *context);
static void config_provider();
static void battery_layer_update_callback(Layer *me, GContext* ctx);
static void pebble_battery_layer_update_callback(Layer *me, GContext* ctx);
static void handle_status_appear(Window *window);
static void handle_status_disappear(Window *window);
static void handle_minute_tick(struct tm* tick_time, TimeUnits units_changed);
static void reset();	
static void swap_bottom_layer();
	
static Window *window;
static PropertyAnimation *ani_out, *ani_in;

static Layer *animated_layer[NUM_LAYERS], *weather_layer;
static Layer *battery_layer, *battery_ind_layer, *calendar_layer;
static Layer *pebble_battery_layer, *pebble_battery_ind_layer;

static TextLayer *text_date_layer, *text_time_layer;

static TextLayer *text_weather_temp_layer, *text_weather_tomorrow_temp_layer, *text_battery_layer;
static TextLayer *calendar_date_layer, *calendar_text_layer, *text_status_layer;
static TextLayer *music_artist_layer, *music_song_layer, *location_street_layer;
 
static BitmapLayer *background_image, *weather_image, *weather_tomorrow_image;
static BitmapLayer *battery_image_layer, *pebble_battery_image_layer;
static BitmapLayer *phone_icon_layer, *pebble_icon_layer;

static int32_t active_layer;
static int32_t updateGPSInterval = GPS_UPDATE_INTERVAL;
static int32_t updateCalandarInterval = 60000;
static int32_t updateWeatherInterval = 60000;
static int32_t updateMusicInterval = 60000;
static bool connected = 0;
static int8_t inTimeOut = 0;
static bool inGPSUpdate = 0;
static bool sending = 0;
static int8_t current_app = -1;
static bool battery_low = false;
static bool pebble_battery_low = false;

static char string_buffer[STRING_LENGTH], location_street_str[STRING_LENGTH], appointment_time[15];
static char weather_cond_str[STRING_LENGTH], weather_tomorrow_temp_str[5], weather_temp_str[5];
static int32_t weather_img, weather_tomorrow_img, batteryPercent, pebble_batteryPercent;

static char calendar_date_str[STRING_LENGTH], calendar_text_str[STRING_LENGTH];
static char music_artist_str[STRING_LENGTH], music_title_str[STRING_LENGTH];


static GBitmap *battery_image, *pebble_battery_image, *phone_icon, *pebble_icon;
static GBitmap *weather_status_small_imgs[NUM_WEATHER_IMAGES];

static AppTimer *timerUpdateCalendar = NULL;
static AppTimer *timerUpdateWeather = NULL;
static AppTimer *timerUpdateMusic = NULL;
static AppTimer *timerSwapBottomLayer = NULL;
static AppTimer *timerUpdateWeatherForecast = NULL;
static AppTimer *timerUpdateGps = NULL;
static AppTimer *timerRecoveryAttempt = NULL;

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

static char local_debug_str[STRING_LENGTH];
#define LOCAL_DEBUG(level, format, ...) \
	do { \
		snprintf(local_debug_str, STRING_LENGTH, format, ## __VA_ARGS__); \
		text_layer_set_text(text_status_layer, local_debug_str); \
		light_enable_interaction(); \
	   } while(0)

static uint32_t s_sequence_number = 0xFFFFFFFE;

/* Convert letter to digit */
static int letter2digit(char letter) {
	if((letter >= 48) && (letter <=57)) {
		return (int)letter - 48;
	}
	
	return -1;
}

/* Convert string to number */
static int32_t string2number(char *string) {
	int32_t result = 0;
	int32_t offset;
	int32_t digit = -1;
	int32_t unit = 1;
	int8_t letter;

	offset = strlen(string) - 1;

	for(unit = 1, result = 0; offset >= 0; unit = unit * 10) {
		letter = string[offset];
		digit = letter2digit(letter);
		if(digit == -1) return -1;
		result = result + (unit * digit);
		offset--;
	}
	
	return result;
}

/* Convert time string ("HH:MM") to number of minutes */
static int32_t timestr2minutes(char *timestr) {
	static char hourStr[] = "00";
	static char minStr[] = "00";
	int32_t hour, min;
	int8_t hDigits = 2;

	//if(DEBUG)
		//LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "timestr2minutes %s", timestr);
	if(timestr[1] == ':') hDigits = 1;
	
	strncpy(hourStr, timestr, hDigits);
	strncpy(minStr, timestr+hDigits+1, 2);
	
	hour = string2number(hourStr);
	if(hour < 0) return -1;
	
	min = string2number(minStr);
	if(min < 0) return -1;
	
	//if(DEBUG)
		//LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "timestr2minutes: %d", (int)(min + (hour * 60)));
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
		//if(DEBUG)
			//LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "apptDisplay: %d, %d", (int)apptInMinutes, (int)timeInMinutes);
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
 
static AppMessageResult sm_message_out_get(DictionaryIterator **iter_out) {
    AppMessageResult result = app_message_outbox_begin(iter_out);
    if(result != APP_MSG_OK) return result;
    dict_write_int32(*iter_out, SM_SEQUENCE_NUMBER_KEY, ++s_sequence_number);
    if(s_sequence_number == 0xFFFFFFFF) {
        s_sequence_number = 1;
    }
	//if(DEBUG)
		//LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "About to send to app");
    return APP_MSG_OK;
}

static void reset_sequence_number() {
	if(DEBUG)
		LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Reseting sequence number");
	if(!bluetooth_connection_service_peek()) return;
	
    DictionaryIterator *iter = NULL;
    app_message_outbox_begin(&iter);
    if(!iter) return;
    dict_write_int32(iter, SM_SEQUENCE_NUMBER_KEY, 0xFFFFFFFF);
    app_message_outbox_send();
}


static void sendCommand(int key) {
	if(DEBUG)
		LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Sending to app: %d", key);
	if(!bluetooth_connection_service_peek()) return;

	if(sending == 1) return;

	DictionaryIterator* iterout = NULL;
	sm_message_out_get(&iterout);
    if(!iterout) return;
	
	if(dict_write_int8(iterout, key, -1) != DICT_OK) return;
	sending = 1;
	app_message_outbox_send();
}


static void sendCommandInt(int key, int param) {
	if(DEBUG)
		LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Sending to app: %d, %d", key, param);
	if(sending == 1) return;

	DictionaryIterator* iterout = NULL;
	sm_message_out_get(&iterout);
    if(!iterout) return;
	
	if(dict_write_int8(iterout, key, param) != DICT_OK) return;
	sending = 1;
	app_message_outbox_send();
}

// Timer callbacks
static void timer_cbk_weather() {
	if(DEBUG)
		LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Weather update callback");

	if(sending) psleep(1000);

	if(timerUpdateWeather) {
		app_timer_cancel(timerUpdateWeather);
		timerUpdateWeather = NULL;
	}
	timerUpdateWeather = app_timer_register(updateWeatherInterval, timer_cbk_weather, NULL);

	if(current_app != WEATHER_APP)
		sendCommandInt(SM_SCREEN_ENTER_KEY, WEATHER_APP);
	sendCommand(SM_STATUS_UPD_WEATHER_KEY);	
}
		
static void timer_cbk_calandar() {
	if(DEBUG)
		LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Calandar update callback");
	if(sending) psleep(1000);
	
	if(timerUpdateCalendar) {
		app_timer_cancel(timerUpdateCalendar);
		timerUpdateCalendar = NULL;
	}
	timerUpdateCalendar = app_timer_register(updateCalandarInterval, timer_cbk_calandar, NULL);

	if(current_app != STATUS_SCREEN_APP)
		sendCommandInt(SM_SCREEN_ENTER_KEY, STATUS_SCREEN_APP);
	sendCommand(SM_STATUS_UPD_CAL_KEY);	
}

static void timer_cbk_music() {
	if(DEBUG)
		LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Music update callback");

	if(sending) psleep(1000);

	if(timerUpdateMusic) {
		app_timer_cancel(timerUpdateMusic);
		timerUpdateMusic = NULL;
	}
	timerUpdateCalendar = app_timer_register(updateMusicInterval, timer_cbk_calandar, NULL);
	
	if(current_app != STATUS_SCREEN_APP)
		sendCommandInt(SM_SCREEN_ENTER_KEY, STATUS_SCREEN_APP);
	sendCommand(SM_SONG_LENGTH_KEY);	
}

static void timer_cbk_layerswap() {
	if(DEBUG)
		LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Swap layers callback");

	swap_bottom_layer();	

	if(timerSwapBottomLayer) {
		app_timer_cancel(timerSwapBottomLayer);
		timerSwapBottomLayer = NULL;
	}
	//timerSwapBottomLayer = app_timer_register(SWAP_BOTTOM_LAYER_INTERVAL, timer_cbk_layerswap, NULL);
}
	
static void timer_cbk_nextdayweather() {
	if(DEBUG)
		LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Next day weather callback");
	if(sending) psleep(1000);

	if(timerUpdateWeatherForecast) {
		app_timer_cancel(timerUpdateWeatherForecast);
		timerUpdateWeatherForecast = NULL;
	}
	
	if(current_app != WEATHER_APP)
		sendCommandInt(SM_SCREEN_ENTER_KEY, WEATHER_APP);
	sendCommand(SM_STATUS_UPD_WEATHER_KEY);	
}
		
static void timer_cbk_gps() {
	if(DEBUG)
		LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "GPS update callback");

	if(sending) psleep(1000);

	sendCommandInt(SM_SCREEN_ENTER_KEY, GPS_APP);
		
	if(timerUpdateGps) {
		app_timer_cancel(timerUpdateGps);
		timerUpdateGps = NULL;
	}
	timerUpdateGps = app_timer_register(updateGPSInterval, timer_cbk_gps, NULL);
}
	
static void timer_cbk_connectionrecover() {
	if(DEBUG)
		LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Recover connection callback");
	if(sending) psleep(1000);

	reset_sequence_number();
	sendCommandInt(SM_SCREEN_ENTER_KEY, STATUS_SCREEN_APP);
}

static void rcv(DictionaryIterator *received, void *context) {
	// Got a message callback
	Tuple *t;
	int8_t interval;

	//if(DEBUG)
		//LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Recieved data from app");
	
	connected = 1;

	t = dict_find(received, SM_COUNT_BATTERY_KEY); 
	if (t!=NULL) {
		batteryPercent = t->value->uint8;
		//if(DEBUG)
			//APP_LOG(APP_LOG_LEVEL_DEBUG, "Battery: %d", (int)batteryPercent);
		if(battery_low && (batteryPercent > 25)) battery_low = false;
		if(!battery_low && (batteryPercent < 20)) {
			battery_low = true;
			vibes_short_pulse();
		}
		layer_mark_dirty(battery_ind_layer);
	}

	t=dict_find(received, SM_WEATHER_TEMP_KEY); 
	if (t!=NULL) {
		memcpy(weather_temp_str, t->value->cstring, 
			   MIN(sizeof(weather_temp_str),strlen(t->value->cstring)));
        weather_temp_str[MIN(sizeof(weather_temp_str),strlen(t->value->cstring))] = '\0';
		//if(DEBUG)
			//LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Temp: %s", weather_temp_str);
		text_layer_set_text(text_weather_temp_layer, weather_temp_str); 
	}

	t=dict_find(received, SM_WEATHER_ICON_KEY); 
	if (t!=NULL) {
		//if(DEBUG)
			//LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Weather: %d", t->value->uint8);
		bitmap_layer_set_bitmap(weather_image, weather_status_small_imgs[t->value->uint8]);	  	
	}

	t=dict_find(received, SM_WEATHER_ICON1_KEY); 
	if (t!=NULL) {
		//if(DEBUG)
			//LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Tomorrow icon: %d", t->value->uint8);
		bitmap_layer_set_bitmap(weather_tomorrow_image, weather_status_small_imgs[t->value->uint8]);	  	
	}
	
	t=dict_find(received, SM_WEATHER_DAY1_KEY); 
	if (t!=NULL) {
		memcpy(weather_tomorrow_temp_str, t->value->cstring + 6,
			   MIN(sizeof(weather_tomorrow_temp_str), strlen(t->value->cstring) - 6));
        weather_tomorrow_temp_str[MIN(sizeof(weather_tomorrow_temp_str), strlen(t->value->cstring) - 6)] = '\0';
		//if(DEBUG)
			//LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Tomorrow: %s", weather_tomorrow_temp_str);
		text_layer_set_text(text_weather_tomorrow_temp_layer, weather_tomorrow_temp_str); 	
	}

	t=dict_find(received, SM_UPDATE_INTERVAL_KEY); 
	if (t!=NULL) {
		//if(DEBUG)
			//LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "GPS interval: %d", (int)t->value->int32);
		if(inGPSUpdate == 1) {
			updateGPSInterval = t->value->int32 * 1000;
			inGPSUpdate = 0;
		}
	}

	t=dict_find(received, SM_GPS_1_KEY); 
	if (t!=NULL) {
		memcpy(location_street_str, t->value->cstring,
			   MIN(sizeof(location_street_str), strlen(t->value->cstring)));
        location_street_str[MIN(sizeof(location_street_str), strlen(t->value->cstring))] = '\0';
		//if(DEBUG)
			//LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Location: %s", location_street_str);
		text_layer_set_text(location_street_layer, location_street_str);
	}

	t=dict_find(received, SM_STATUS_CAL_TIME_KEY); 
	if (t!=NULL) {
		memcpy(calendar_date_str, t->value->cstring,
			   MIN(sizeof(calendar_date_str), strlen(t->value->cstring)));
        calendar_date_str[MIN(sizeof(calendar_date_str), strlen(t->value->cstring))] = '\0';
		//text_layer_set_text(text_status_layer, calendar_date_str); 	
		//if(DEBUG)
			//LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Calendar date: %s", calendar_date_str);
		strncpy(appointment_time, calendar_date_str, 11);
		apptDisplay();
	}

	t=dict_find(received, SM_STATUS_CAL_TEXT_KEY); 
	if (t!=NULL) {
		memcpy(calendar_text_str, t->value->cstring,
			   MIN(sizeof(calendar_text_str), strlen(t->value->cstring)));
        calendar_text_str[MIN(sizeof(calendar_text_str), strlen(t->value->cstring))] = '\0';
		text_layer_set_text(calendar_text_layer, calendar_text_str); 	
		//if(DEBUG)
			//LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Calendar: %s", calendar_text_str);
		
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
			   MIN(sizeof(music_artist_str), strlen(t->value->cstring)));
        music_artist_str[MIN(sizeof(music_artist_str), strlen(t->value->cstring))] = '\0';
		//if(DEBUG)
			//LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Artist: %s", music_artist_str);
		text_layer_set_text(music_artist_layer, music_artist_str); 	
	}

	t=dict_find(received, SM_STATUS_MUS_TITLE_KEY); 
	if (t!=NULL) {
		memcpy(music_title_str, t->value->cstring,
			   MIN(sizeof(music_title_str), strlen(t->value->cstring)));
        music_title_str[MIN(sizeof(music_title_str), strlen(t->value->cstring))] = '\0';
		//if(DEBUG)
			//LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Song: %s", music_title_str);
		text_layer_set_text(music_song_layer, music_title_str); 	
	}

	t=dict_find(received, SM_STATUS_UPD_WEATHER_KEY); 
	if (t!=NULL) {
			updateWeatherInterval = t->value->int32 * 1000;
		//if(DEBUG)
			//LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Weather interval: %d", (int)t->value->int32);

		if(timerUpdateWeather) {
			app_timer_cancel(timerUpdateWeather);
			timerUpdateWeather = NULL;
		}
		timerUpdateWeather = app_timer_register(updateWeatherInterval, timer_cbk_weather, NULL);
	}

	t=dict_find(received, SM_STATUS_UPD_CAL_KEY); 
	if (t!=NULL) {
		updateCalandarInterval = t->value->int32 * 1000;
		//if(DEBUG)
			//LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Calandar interval: %d", (int)t->value->int32);

		if(timerUpdateCalendar) {
			app_timer_cancel(timerUpdateCalendar);
			timerUpdateCalendar = NULL;
		}
		timerUpdateCalendar = app_timer_register(updateCalandarInterval, timer_cbk_calandar, NULL);
	}

	t=dict_find(received, SM_SONG_LENGTH_KEY); 
	if (t!=NULL) {
		updateMusicInterval = t->value->int32 * 1000;
		//if(DEBUG)
			//LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Music interval: %d", (int)t->value->int32);

		if(timerUpdateMusic) {
			app_timer_cancel(timerUpdateMusic);
			timerUpdateMusic = NULL;
		}
		timerUpdateMusic = app_timer_register(updateMusicInterval, timer_cbk_music, NULL);
	}
	
	if(!DEBUG)
		text_layer_set_text(text_status_layer, "");

}

static void dropped(AppMessageResult reason, void *context){
	if(DEBUG)
		LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Message dropper: %d", reason);

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

static void sent_ok(DictionaryIterator *sent, void *context) {
	//if(DEBUG)
		//LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Message sent to app");

	Tuple *t;
	
	if(timerRecoveryAttempt) {
		app_timer_cancel(timerRecoveryAttempt);
		timerRecoveryAttempt = NULL;
	}

	t = dict_find(sent, SM_SCREEN_ENTER_KEY);
	if(t) current_app = t->value->int8;

	sending = 0;
	if(!DEBUG)
		text_layer_set_text(text_status_layer, "Ok");
	
	connected = 1;
	inTimeOut = 0;
}

static void send_failed(DictionaryIterator *failed, AppMessageResult reason, void *context) {
	if(DEBUG)
		LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Message failed to send: %d", reason);

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


static void select_single_click_handler(ClickRecognizerRef recognizer, void *context) {
	sendCommand(SM_PLAYPAUSE_KEY);
}

static void select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
	sendCommand(SM_FIND_MY_PHONE_KEY);
}

static void select_up_handler(ClickRecognizerRef recognizer, void *context) {
}


static void select_down_handler(ClickRecognizerRef recognizer, void *context) {
}


static void up_single_click_handler(ClickRecognizerRef recognizer, void *context) {
	sendCommand(SM_VOLUME_UP_KEY);
}

void down_single_click_handler(ClickRecognizerRef recognizer, void *context) {
	sendCommand(SM_VOLUME_DOWN_KEY);
}

static void swap_bottom_layer() {
	ani_out = property_animation_create_layer_frame(animated_layer[active_layer], &GRect(30, 72, 75, 50), &GRect(-75, 72, 75, 50));
	animation_schedule(&(ani_out->animation));

	active_layer = (active_layer + 1) % (NUM_LAYERS);

	ani_in = property_animation_create_layer_frame(animated_layer[active_layer], &GRect(144, 72, 75, 50), &GRect(30, 72, 75, 50));
	animation_schedule(&(ani_in->animation));
}


static void config_provider() {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_single_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 0, select_long_click_handler, NULL);
  window_single_click_subscribe(BUTTON_ID_UP, up_single_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_single_click_handler);
}


static void battery_layer_update_callback(Layer *me, GContext* ctx) {
	//draw the remaining battery percentage
	graphics_context_set_stroke_color(ctx, GColorBlack);
	graphics_context_set_fill_color(ctx, GColorWhite);

	graphics_fill_rect(ctx, GRect(2+16-(int)((batteryPercent/100.0)*16.0), 2, (int)((batteryPercent/100.0)*16.0), 8), 0, GCornerNone);
}

static void pebble_battery_layer_update_callback(Layer *me, GContext* ctx) {
	//draw the remaining battery percentage
	graphics_context_set_stroke_color(ctx, GColorBlack);
	graphics_context_set_fill_color(ctx, GColorWhite);

	graphics_fill_rect(ctx, GRect(2+16-(int)((pebble_batteryPercent/100.0)*16.0), 2, (int)((pebble_batteryPercent/100.0)*16.0), 8), 0, GCornerNone);
}

static void window_load(Window *this) {
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

	text_status_layer = text_layer_create(GRect(6, 110, 138, 20));
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
	// timerSwapBottomLayer = app_timer_register(SWAP_BOTTOM_LAYER_INTERVAL, timer_cbk_layerswap, NULL);
	timerUpdateGps = app_timer_register(updateGPSInterval, timer_cbk_gps, NULL);
	timerUpdateMusic = app_timer_register(DEFAULT_SONG_UPDATE_INTERVAL, timer_cbk_music, NULL);
	timerUpdateWeather = app_timer_register(updateWeatherInterval, timer_cbk_weather, NULL);
	timerUpdateCalendar = app_timer_register(updateCalandarInterval, timer_cbk_calandar, NULL);
}

static void pebble_battery_update(BatteryChargeState pb_bat) {
	pebble_batteryPercent = pb_bat.charge_percent;
	if(pebble_battery_low && (pebble_batteryPercent > 25)) pebble_battery_low = false;
	if(!pebble_battery_low && (pebble_batteryPercent < 20)) {
		pebble_battery_low = true;
		vibes_short_pulse();
	}
	layer_mark_dirty(pebble_battery_layer);
}

static void bluetooth_connection_handler(bool btConnected) {
	if(btConnected) {
		text_layer_set_text(text_status_layer, "");
		sendCommandInt(SM_SCREEN_ENTER_KEY, STATUS_SCREEN_APP);
		
		if(!timerUpdateWeather)
			timerUpdateWeather = app_timer_register(updateWeatherInterval, timer_cbk_weather, NULL);
		if(!timerUpdateCalendar)
			timerUpdateCalendar = app_timer_register(updateCalandarInterval, timer_cbk_calandar, NULL);
		//if(!timerSwapBottomLayer)
			//timerSwapBottomLayer = app_timer_register(SWAP_BOTTOM_LAYER_INTERVAL, timer_cbk_layerswap, NULL);
		if(!timerUpdateGps)
			timerUpdateGps = app_timer_register(updateGPSInterval, timer_cbk_gps, NULL);
		if(!timerUpdateMusic)
			timerUpdateMusic = app_timer_register(DEFAULT_SONG_UPDATE_INTERVAL, timer_cbk_music, NULL);
	} else {
		text_layer_set_text(text_status_layer, "No BT");
		
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

static void accel_tep_handler(AccelAxisType axis, int32_t direction) {
	if((axis == ACCEL_AXIS_Y) && (direction == -1))
		swap_bottom_layer();
}

static void handle_minute_tick(struct tm* tick_time, TimeUnits units_changed) {
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

static void window_unload(Window *this) {
	if(DEBUG)
		LOCAL_DEBUG(APP_LOG_LEVEL_DEBUG, "Unloading main window");
	
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
	// Create app's base window
	window = window_create();
	window_set_window_handlers(window, (WindowHandlers) {
		.load = window_load,
		.unload = window_unload,
	});
	window_set_click_config_provider(window, (ClickConfigProvider) config_provider);

	// Push the main window onto the stack
	const bool animated = true;
	window_set_fullscreen(window, true);
	window_stack_push(window, animated);
	window_set_background_color(window, GColorBlack);

	// Subscribe to required services
	tick_timer_service_subscribe(MINUTE_UNIT, handle_minute_tick);
	battery_state_service_subscribe(pebble_battery_update);
	bluetooth_connection_service_subscribe(bluetooth_connection_handler);
	accel_tap_service_subscribe(accel_tep_handler);

	// Initialize messaging
	app_message_register_inbox_received(rcv);
	app_message_register_inbox_dropped(dropped);
	app_message_register_outbox_sent(sent_ok);
	app_message_register_outbox_failed(send_failed);
	const uint32_t inbound_size = app_message_inbox_size_maximum();
	const uint32_t outbound_size = app_message_outbox_size_maximum();
	app_message_open(inbound_size, outbound_size);

	// Init global variables
	appointment_time[0] = '\0';
}

// Release resources
static void do_deinit(void) {
	// Unsubscribe services
	tick_timer_service_unsubscribe();
	battery_state_service_unsubscribe();
	bluetooth_connection_service_unsubscribe();
	accel_tap_service_unsubscribe();
	
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