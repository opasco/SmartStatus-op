#include "pebble_os.h"
#include "pebble_app.h"
#include "pebble_fonts.h"

#include "globals.h"


#define MY_UUID { 0x91, 0x41, 0xB6, 0x28, 0xBC, 0x89, 0x49, 0x8E, 0xB1, 0x47, 0x04, 0x9F, 0x49, 0xC0, 0x99, 0xAD }

PBL_APP_INFO(MY_UUID,
             "SmartStatus-op", "Olivier Pasco",
             1, 0, /* App version */
             RESOURCE_ID_APP_ICON,
             APP_INFO_STANDARD_APP);

#define STRING_LENGTH 255
#define NUM_WEATHER_IMAGES	8
#define SWAP_BOTTOM_LAYER_INTERVAL 15000
#define GPS_UPDATE_INTERVAL 60000

typedef enum {MUSIC_LAYER, LOCATION_LAYER, NUM_LAYERS} AnimatedLayers;


AppMessageResult sm_message_out_get(DictionaryIterator **iter_out);
void reset_sequence_number();
char* int_to_str(int num, char *outbuf);
void sendCommand(int key);
void sendCommandInt(int key, int param);
void rcv(DictionaryIterator *received, void *context);
void dropped(void *context, AppMessageResult reason);
void select_up_handler(ClickRecognizerRef recognizer, Window *window);
void select_down_handler(ClickRecognizerRef recognizer, Window *window);
void up_single_click_handler(ClickRecognizerRef recognizer, Window *window);
void down_single_click_handler(ClickRecognizerRef recognizer, Window *window);
void config_provider(ClickConfig **config, Window *window);
void battery_layer_update_callback(Layer *me, GContext* ctx);
void handle_status_appear(Window *window);
void handle_status_disappear(Window *window);
void handle_init(AppContextRef ctx);
void handle_minute_tick(AppContextRef ctx, PebbleTickEvent *t);
void handle_deinit(AppContextRef ctx);	
void reset();	
	
AppContextRef g_app_context;


static Window window;
static PropertyAnimation ani_out, ani_in;

static Layer animated_layer[NUM_LAYERS], weather_layer;
static Layer battery_layer, battery_ind_layer, calendar_layer;

static TextLayer text_date_layer, text_time_layer;

static TextLayer text_weather_cond_layer, text_weather_temp_layer, text_weather_tomorrow_temp_layer, text_battery_layer;
static TextLayer calendar_date_layer, calendar_text_layer, text_status_layer;
static TextLayer music_artist_layer, music_song_layer, location_street_layer;
 
static BitmapLayer background_image, weather_image, weather_tomorrow_image, battery_image_layer;

static int active_layer;
static int connected = 0;

static char string_buffer[STRING_LENGTH], location_street_str[STRING_LENGTH];
static char weather_cond_str[STRING_LENGTH], weather_tomorrow_temp_str[STRING_LENGTH], weather_temp_str[5];
static int weather_img, weather_tomorrow_img, batteryPercent;

static char calendar_date_str[STRING_LENGTH], calendar_text_str[STRING_LENGTH];
static char music_artist_str[STRING_LENGTH], music_title_str[STRING_LENGTH];


HeapBitmap bg_image, battery_image;
HeapBitmap weather_status_imgs[NUM_WEATHER_IMAGES];
HeapBitmap weather_status_small_imgs[NUM_WEATHER_IMAGES];

static AppTimerHandle timerUpdateCalendar = 0;
static AppTimerHandle timerUpdateWeather = 0;
static AppTimerHandle timerUpdateMusic = 0;
static AppTimerHandle timerSwapBottomLayer = 0;
static AppTimerHandle timerUpdateWeatherForecast = 0;
static AppTimerHandle timerUpdateGps = 0;

/*
const int WEATHER_IMG_IDS[] = {
  RESOURCE_ID_IMAGE_SUN,
  RESOURCE_ID_IMAGE_RAIN,
  RESOURCE_ID_IMAGE_CLOUD,
  RESOURCE_ID_IMAGE_SUN_CLOUD,
  RESOURCE_ID_IMAGE_FOG,
  RESOURCE_ID_IMAGE_WIND,
  RESOURCE_ID_IMAGE_SNOW,
  RESOURCE_ID_IMAGE_THUNDER
};
*/

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



static uint32_t s_sequence_number = 0xFFFFFFFE;

AppMessageResult sm_message_out_get(DictionaryIterator **iter_out) {
    AppMessageResult result = app_message_out_get(iter_out);
    if(result != APP_MSG_OK) return result;
    dict_write_int32(*iter_out, SM_SEQUENCE_NUMBER_KEY, ++s_sequence_number);
    if(s_sequence_number == 0xFFFFFFFF) {
        s_sequence_number = 1;
    }
    return APP_MSG_OK;
}

void reset_sequence_number() {
    DictionaryIterator *iter = NULL;
    app_message_out_get(&iter);
    if(!iter) return;
    dict_write_int32(iter, SM_SEQUENCE_NUMBER_KEY, 0xFFFFFFFF);
    app_message_out_send();
    app_message_out_release();
}


char* int_to_str(int num, char *outbuf) {
	int digit, i=0, j=0;
	char buf[STRING_LENGTH];
	bool negative=false;
	
	if (num < 0) {
		negative = true;
		num = -1 * num;
	}
	
	for (i=0; i<STRING_LENGTH; i++) {
		digit = num % 10;
		if ((num==0) && (i>0)) 
			break;
		else
			buf[i] = '0' + digit;
		 
		num/=10;
	}
	
	if (negative)
		buf[i++] = '-';
	
	buf[i--] = '\0';
	
	
	while (i>=0) {
		outbuf[j++] = buf[i--];
	}

	outbuf[j++] = '%';
	outbuf[j] = '\0';
	
	return outbuf;
}


void sendCommand(int key) {
	DictionaryIterator* iterout;
	sm_message_out_get(&iterout);
    if(!iterout) return;
	
	dict_write_int8(iterout, key, -1);
	app_message_out_send();
	app_message_out_release();	
}


void sendCommandInt(int key, int param) {
	DictionaryIterator* iterout;
	sm_message_out_get(&iterout);
    if(!iterout) return;
	
	dict_write_int8(iterout, key, param);
	app_message_out_send();
	app_message_out_release();	
}


void rcv(DictionaryIterator *received, void *context) {
	// Got a message callback
	Tuple *t;

	text_layer_set_text(&text_status_layer, "Ok");
	connected =1;

	t=dict_find(received, SM_WEATHER_COND_KEY); 
	if (t!=NULL) {
		memcpy(weather_cond_str, t->value->cstring, strlen(t->value->cstring));
        weather_cond_str[strlen(t->value->cstring)] = '\0';
		text_layer_set_text(&text_weather_cond_layer, weather_cond_str); 	
	}

	t=dict_find(received, SM_WEATHER_TEMP_KEY); 
	if (t!=NULL) {
		memcpy(weather_temp_str, t->value->cstring, strlen(t->value->cstring));
        weather_temp_str[strlen(t->value->cstring)] = '\0';
		text_layer_set_text(&text_weather_temp_layer, weather_temp_str); 
		
		layer_set_hidden(&text_weather_cond_layer.layer, true);
		layer_set_hidden(&text_weather_temp_layer.layer, false);
			
	}

	t=dict_find(received, SM_WEATHER_ICON_KEY); 
	if (t!=NULL) {
		bitmap_layer_set_bitmap(&weather_image, &weather_status_small_imgs[t->value->uint8].bmp);	  	
	}

	t=dict_find(received, SM_WEATHER_ICON1_KEY); 
	if (t!=NULL) {
		bitmap_layer_set_bitmap(&weather_tomorrow_image, &weather_status_small_imgs[t->value->uint8].bmp);	  	
	}
	
	t=dict_find(received, SM_WEATHER_DAY1_KEY); 
	if (t!=NULL) {
		memcpy(weather_tomorrow_temp_str, t->value->cstring + 6, strlen(t->value->cstring));
        weather_tomorrow_temp_str[strlen(t->value->cstring)] = '\0';
		text_layer_set_text(&text_weather_tomorrow_temp_layer, weather_tomorrow_temp_str); 	
	}

	t=dict_find(received, SM_GPS_1_KEY); 
	if (t!=NULL) {
		text_layer_set_text(&location_street_layer, "Reading Location");

		memcpy(location_street_str, t->value->cstring, strlen(t->value->cstring));
        location_street_str[strlen(t->value->cstring)] = '\0';
		text_layer_set_text(&location_street_layer, location_street_str); 	

	}

	t=dict_find(received, SM_COUNT_BATTERY_KEY); 
	if (t!=NULL) {
		batteryPercent = t->value->uint8;
		layer_mark_dirty(&battery_ind_layer);
		text_layer_set_text(&text_battery_layer, int_to_str(batteryPercent, string_buffer) ); 	
	}

	t=dict_find(received, SM_STATUS_CAL_TIME_KEY); 
	if (t!=NULL) {
		memcpy(calendar_date_str, t->value->cstring, strlen(t->value->cstring));
        calendar_date_str[strlen(t->value->cstring)] = '\0';
		text_layer_set_text(&calendar_date_layer, calendar_date_str); 	
	}

	t=dict_find(received, SM_STATUS_CAL_TEXT_KEY); 
	if (t!=NULL) {
		memcpy(calendar_text_str, t->value->cstring, strlen(t->value->cstring));
        calendar_text_str[strlen(t->value->cstring)] = '\0';
		text_layer_set_text(&calendar_text_layer, calendar_text_str); 	
	}


	t=dict_find(received, SM_STATUS_MUS_ARTIST_KEY); 
	if (t!=NULL) {
		memcpy(music_artist_str, t->value->cstring, strlen(t->value->cstring));
        music_artist_str[strlen(t->value->cstring)] = '\0';
		text_layer_set_text(&music_artist_layer, music_artist_str); 	
	}

	t=dict_find(received, SM_STATUS_MUS_TITLE_KEY); 
	if (t!=NULL) {
		memcpy(music_title_str, t->value->cstring, strlen(t->value->cstring));
        music_title_str[strlen(t->value->cstring)] = '\0';
		text_layer_set_text(&music_song_layer, music_title_str); 	
	}

	t=dict_find(received, SM_STATUS_UPD_WEATHER_KEY); 
	if (t!=NULL) {
		int interval = t->value->int32 * 1000;

		app_timer_cancel_event(g_app_context, timerUpdateWeather);
		timerUpdateWeather = app_timer_send_event(g_app_context, interval /* milliseconds */, 1);
	}

	t=dict_find(received, SM_STATUS_UPD_CAL_KEY); 
	if (t!=NULL) {
		int interval = t->value->int32 * 1000;

		app_timer_cancel_event(g_app_context, timerUpdateCalendar);
		timerUpdateCalendar = app_timer_send_event(g_app_context, interval /* milliseconds */, 2);
	}

	t=dict_find(received, SM_SONG_LENGTH_KEY); 
	if (t!=NULL) {
		int interval = t->value->int32 * 1000;

		app_timer_cancel_event(g_app_context, timerUpdateMusic);
		timerUpdateMusic = app_timer_send_event(g_app_context, interval /* milliseconds */, 3);
	}

}

void dropped(void *context, AppMessageResult reason){
	// DO SOMETHING WITH THE DROPPED REASON / DISPLAY AN ERROR / RESEND 
}



void select_single_click_handler(ClickRecognizerRef recognizer, Window *window) {
  (void)recognizer;
  (void)window;

	sendCommand(SM_PLAYPAUSE_KEY);
}

void select_long_click_handler(ClickRecognizerRef recognizer, Window *window) {
  (void)recognizer;
  (void)window;

	sendCommand(SM_FIND_MY_PHONE_KEY);
}

void select_up_handler(ClickRecognizerRef recognizer, Window *window) {
  (void)recognizer;
  (void)window;

	//revert to showing the temperature 
	layer_set_hidden(&text_weather_temp_layer.layer, false);
	layer_set_hidden(&text_weather_cond_layer.layer, true);

}


void select_down_handler(ClickRecognizerRef recognizer, Window *window) {
  (void)recognizer;
  (void)window;

	//show the weather condition instead of temperature while center button is pressed
	layer_set_hidden(&text_weather_temp_layer.layer, true);
	layer_set_hidden(&text_weather_cond_layer.layer, false);

}


void up_single_click_handler(ClickRecognizerRef recognizer, Window *window) {
  (void)recognizer;
  (void)window;

	sendCommand(SM_NEXT_TRACK_KEY);
}

void swap_bottom_layer() {
	//on a press of the bottom button, scroll in the next layer

	property_animation_init_layer_frame(&ani_out, &animated_layer[active_layer], &GRect(30, 79, 75, 40), &GRect(-75, 79, 75, 40));
	animation_schedule(&(ani_out.animation));


	active_layer = (active_layer + 1) % (NUM_LAYERS);


	property_animation_init_layer_frame(&ani_in, &animated_layer[active_layer], &GRect(144, 79, 75, 40), &GRect(30, 79, 75, 40));
	animation_schedule(&(ani_in.animation));
}

void down_single_click_handler(ClickRecognizerRef recognizer, Window *window) {
  (void)recognizer;
  (void)window;

	sendCommand(SM_PREVIOUS_TRACK_KEY);
}


void config_provider(ClickConfig **config, Window *window) {
  (void)window;


  config[BUTTON_ID_SELECT]->click.handler = (ClickHandler) select_single_click_handler;
//  config[BUTTON_ID_SELECT]->raw.up_handler = (ClickHandler) select_up_handler;
//  config[BUTTON_ID_SELECT]->raw.down_handler = (ClickHandler) select_down_handler;

  config[BUTTON_ID_SELECT]->long_click.handler = (ClickHandler) select_long_click_handler;
//  config[BUTTON_ID_SELECT]->long_click.release_handler = (ClickHandler) select_long_release_handler;


  config[BUTTON_ID_UP]->click.handler = (ClickHandler) up_single_click_handler;
  config[BUTTON_ID_UP]->click.repeat_interval_ms = 100;
//  config[BUTTON_ID_UP]->long_click.handler = (ClickHandler) up_long_click_handler;
//  config[BUTTON_ID_UP]->long_click.release_handler = (ClickHandler) up_long_release_handler;

  config[BUTTON_ID_DOWN]->click.handler = (ClickHandler) down_single_click_handler;
  config[BUTTON_ID_DOWN]->click.repeat_interval_ms = 100;
//  config[BUTTON_ID_DOWN]->long_click.handler = (ClickHandler) down_long_click_handler;
//  config[BUTTON_ID_DOWN]->long_click.release_handler = (ClickHandler) down_long_release_handler;

}


void battery_layer_update_callback(Layer *me, GContext* ctx) {
	
	//draw the remaining battery percentage
	graphics_context_set_stroke_color(ctx, GColorBlack);
	graphics_context_set_fill_color(ctx, GColorWhite);

	graphics_fill_rect(ctx, GRect(2+16-(int)((batteryPercent/100.0)*16.0), 2, (int)((batteryPercent/100.0)*16.0), 8), 0, GCornerNone);
	
}


void handle_status_appear(Window *window)
{
	sendCommandInt(SM_SCREEN_ENTER_KEY, STATUS_SCREEN_APP);
}

void handle_status_disappear(Window *window)
{
	sendCommandInt(SM_SCREEN_EXIT_KEY, STATUS_SCREEN_APP);
	
	app_timer_cancel_event(g_app_context, timerUpdateCalendar);
	app_timer_cancel_event(g_app_context, timerUpdateMusic);
	app_timer_cancel_event(g_app_context, timerUpdateWeather);
	app_timer_cancel_event(g_app_context, timerSwapBottomLayer);
	app_timer_cancel_event(g_app_context, timerUpdateGps);
}

void reset() {
	
	layer_set_hidden(&text_weather_temp_layer.layer, true);
	layer_set_hidden(&text_weather_cond_layer.layer, false);
	text_layer_set_text(&text_weather_cond_layer, "Updating..."); 	
	
}

void handle_init(AppContextRef ctx) {
	(void)ctx;

  g_app_context = ctx;

	window_init(&window, "Window Name");
	window_set_window_handlers(&window, (WindowHandlers) {
	    .appear = (WindowHandler)handle_status_appear,
	    .disappear = (WindowHandler)handle_status_disappear
	});

	window_stack_push(&window, true /* Animated */);
	window_set_fullscreen(&window, true);

	resource_init_current_app(&APP_RESOURCES);


	//init weather images
	for (int i=0; i<NUM_WEATHER_IMAGES; i++) {
		//heap_bitmap_init(&weather_status_imgs[i], WEATHER_IMG_IDS[i]);
		heap_bitmap_init(&weather_status_small_imgs[i], WEATHER_SMALL_IMG_IDS[i]);
	}
	
	heap_bitmap_init(&bg_image, RESOURCE_ID_IMAGE_BACKGROUND);


	//init background image
	bitmap_layer_init(&background_image, GRect(0, 0, 144, 168));
	layer_add_child(&window.layer, &background_image.layer);
	bitmap_layer_set_bitmap(&background_image, &bg_image.bmp);

	// init battery layer
	layer_init(&battery_layer, GRect(95, 45, 49, 45));
	layer_add_child(&window.layer, &battery_layer);

	heap_bitmap_init(&battery_image, RESOURCE_ID_IMAGE_BATTERY);

	bitmap_layer_init(&battery_image_layer, GRect(12, 8, 23, 14));
	layer_add_child(&battery_layer, &battery_image_layer.layer);
	bitmap_layer_set_bitmap(&battery_image_layer, &battery_image.bmp);


	text_layer_init(&text_battery_layer, GRect(4, 20, 40, 60));
	text_layer_set_text_alignment(&text_battery_layer, GTextAlignmentCenter);
	text_layer_set_text_color(&text_battery_layer, GColorWhite);
	text_layer_set_background_color(&text_battery_layer, GColorClear);
	text_layer_set_font(&text_battery_layer,  fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
	// layer_add_child(&battery_layer, &text_battery_layer.layer);
	text_layer_set_text(&text_battery_layer, "-");


	layer_init(&battery_ind_layer, GRect(14, 9, 19, 11));
	battery_ind_layer.update_proc = &battery_layer_update_callback;
	layer_add_child(&battery_layer, &battery_ind_layer);

	batteryPercent = 100;
	layer_mark_dirty(&battery_ind_layer);

	//init weather layer and add weather image, weather condition, temperature
	layer_init(&weather_layer, GRect(0, 78, 144, 45));
	layer_add_child(&window.layer, &weather_layer);


	text_layer_init(&text_weather_cond_layer, GRect(48, 1, 48, 40)); // GRect(5, 2, 47, 40)
	text_layer_set_text_alignment(&text_weather_cond_layer, GTextAlignmentCenter);
	text_layer_set_text_color(&text_weather_cond_layer, GColorWhite);
	text_layer_set_background_color(&text_weather_cond_layer, GColorClear);
	text_layer_set_font(&text_weather_cond_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
//	layer_add_child(&weather_layer, &text_weather_cond_layer.layer);

	layer_set_hidden(&text_weather_cond_layer.layer, false);
	text_layer_set_text(&text_weather_cond_layer, "Updating..."); 	
	

	weather_img = 0;

	bitmap_layer_init(&weather_image, GRect(5, 4, 20, 20)); // GRect(52, 2, 40, 40)
	layer_add_child(&weather_layer, &weather_image.layer);
	bitmap_layer_set_bitmap(&weather_image, &weather_status_small_imgs[0].bmp);

	weather_tomorrow_img = 0;

	bitmap_layer_init(&weather_tomorrow_image, GRect(112, 4, 20, 20)); // GRect(52, 2, 40, 40)
	layer_add_child(&weather_layer, &weather_tomorrow_image.layer);
	bitmap_layer_set_bitmap(&weather_tomorrow_image, &weather_status_small_imgs[0].bmp);

	text_layer_init(&text_weather_tomorrow_temp_layer, GRect(105, 23, 31, 20)); // GRect(5, 2, 47, 40)
	text_layer_set_text_alignment(&text_weather_tomorrow_temp_layer, GTextAlignmentCenter);
	text_layer_set_text_color(&text_weather_tomorrow_temp_layer, GColorWhite);
	text_layer_set_background_color(&text_weather_tomorrow_temp_layer, GColorClear);
	text_layer_set_font(&text_weather_tomorrow_temp_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
	layer_add_child(&weather_layer, &text_weather_tomorrow_temp_layer.layer);
	text_layer_set_text(&text_weather_tomorrow_temp_layer, "../.."); 	
	
	text_layer_init(&text_weather_temp_layer, GRect(5, 23, 25, 20)); // GRect(98, 4, 47, 40)
	text_layer_set_text_alignment(&text_weather_temp_layer, GTextAlignmentCenter);
	text_layer_set_text_color(&text_weather_temp_layer, GColorWhite);
	text_layer_set_background_color(&text_weather_temp_layer, GColorClear);
	text_layer_set_font(&text_weather_temp_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
	layer_add_child(&weather_layer, &text_weather_temp_layer.layer);
	text_layer_set_text(&text_weather_temp_layer, "-°"); 	
	
	//init layers for time and date and status
	text_layer_init(&text_date_layer, window.layer.frame);
	text_layer_set_text_alignment(&text_date_layer, GTextAlignmentLeft);
	text_layer_set_text_color(&text_date_layer, GColorWhite);
	text_layer_set_background_color(&text_date_layer, GColorClear);
	layer_set_frame(&text_date_layer.layer, GRect(6, 48, 50, 30));
	//text_layer_set_font(&text_date_layer, fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_ROBOTO_CONDENSED_21)));
	text_layer_set_font(&text_date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
	layer_add_child(&window.layer, &text_date_layer.layer);


	text_layer_init(&text_time_layer, window.layer.frame);
	text_layer_set_text_alignment(&text_time_layer, GTextAlignmentCenter);
	text_layer_set_text_color(&text_time_layer, GColorWhite);
	text_layer_set_background_color(&text_time_layer, GColorClear);
	layer_set_frame(&text_time_layer.layer, GRect(0, -5, 144, 50));
	text_layer_set_font(&text_time_layer, fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_ROBOTO_BOLD_SUBSET_49)));
	layer_add_child(&window.layer, &text_time_layer.layer);

	text_layer_init(&text_status_layer, GRect(52, 53, 45, 20));
	text_layer_set_text_alignment(&text_status_layer, GTextAlignmentCenter);
	text_layer_set_text_color(&text_status_layer, GColorWhite);
	text_layer_set_background_color(&text_status_layer, GColorClear);
	text_layer_set_font(&text_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
	layer_add_child(&window.layer, &text_status_layer.layer);
	text_layer_set_text(&text_status_layer, "Init.");


	//init calendar layer
	layer_init(&calendar_layer, GRect(0, 124, 144, 45));
	layer_add_child(&window.layer, &calendar_layer);
	
	text_layer_init(&calendar_date_layer, GRect(6, 0, 132, 21));
	text_layer_set_text_alignment(&calendar_date_layer, GTextAlignmentLeft);
	text_layer_set_text_color(&calendar_date_layer, GColorWhite);
	text_layer_set_background_color(&calendar_date_layer, GColorClear);
	text_layer_set_font(&calendar_date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
	layer_add_child(&calendar_layer, &calendar_date_layer.layer);
	text_layer_set_text(&calendar_date_layer, "No Upcoming"); 	


	text_layer_init(&calendar_text_layer, GRect(6, 15, 132, 28));
	text_layer_set_text_alignment(&calendar_text_layer, GTextAlignmentLeft);
	text_layer_set_text_color(&calendar_text_layer, GColorWhite);
	text_layer_set_background_color(&calendar_text_layer, GColorClear);
	text_layer_set_font(&calendar_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
	layer_add_child(&calendar_layer, &calendar_text_layer.layer);
	text_layer_set_text(&calendar_text_layer, "Appointment");
	
	
	
	//init music layer
	layer_init(&animated_layer[MUSIC_LAYER], GRect(30, 79, 75, 40));
	layer_add_child(&window.layer, &animated_layer[MUSIC_LAYER]);
	
	text_layer_init(&music_artist_layer, GRect(0, 0, 75, 20));
	text_layer_set_text_alignment(&music_artist_layer, GTextAlignmentLeft);
	text_layer_set_text_color(&music_artist_layer, GColorWhite);
	text_layer_set_background_color(&music_artist_layer, GColorClear);
	text_layer_set_font(&music_artist_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
	layer_add_child(&animated_layer[MUSIC_LAYER], &music_artist_layer.layer);
	text_layer_set_text(&music_artist_layer, "No Artist"); 	


	text_layer_init(&music_song_layer, GRect(0, 21, 75, 20));
	text_layer_set_text_alignment(&music_song_layer, GTextAlignmentLeft);
	text_layer_set_text_color(&music_song_layer, GColorWhite);
	text_layer_set_background_color(&music_song_layer, GColorClear);
	text_layer_set_font(&music_song_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
	layer_add_child(&animated_layer[MUSIC_LAYER], &music_song_layer.layer);
	text_layer_set_text(&music_song_layer, "No Title");
	
	
	//init location layer
	layer_init(&animated_layer[LOCATION_LAYER], GRect(144, 798, 75, 47));
	layer_add_child(&window.layer, &animated_layer[LOCATION_LAYER]);
	
	text_layer_init(&location_street_layer, GRect(0, 0, 75, 47));
	text_layer_set_text_alignment(&location_street_layer, GTextAlignmentCenter);
	text_layer_set_text_color(&location_street_layer, GColorWhite);
	text_layer_set_background_color(&location_street_layer, GColorClear);
	text_layer_set_font(&location_street_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
	layer_add_child(&animated_layer[LOCATION_LAYER], &location_street_layer.layer);
	text_layer_set_text(&location_street_layer, "Unknown Location"); 	


	window_set_click_config_provider(&window, (ClickConfigProvider) config_provider);

	active_layer = MUSIC_LAYER;
	
	timerSwapBottomLayer = app_timer_send_event(g_app_context, SWAP_BOTTOM_LAYER_INTERVAL /* milliseconds */, 4);
	timerUpdateWeatherForecast = app_timer_send_event(g_app_context, 5000 /* milliseconds */, 5);
	timerUpdateGps = app_timer_send_event(g_app_context, GPS_UPDATE_INTERVAL /* milliseconds */, 6);

	reset();
}



void handle_minute_tick(AppContextRef ctx, PebbleTickEvent *t) {
/* Display the time */
  (void)ctx;

  static char time_text[] = "00:00";
  static char date_text[] = "Xxxxxxxxx 00";

  char *time_format;

  string_format_time(date_text, sizeof(date_text), "%b %e", t->tick_time);
  text_layer_set_text(&text_date_layer, date_text);


  if (clock_is_24h_style()) {
    time_format = "%R";
  } else {
    time_format = "%I:%M";
  }

  string_format_time(time_text, sizeof(time_text), time_format, t->tick_time);

  if (!clock_is_24h_style() && (time_text[0] == '0')) {
    memmove(time_text, &time_text[1], sizeof(time_text) - 1);
  }

  text_layer_set_text(&text_time_layer, time_text);
	
	// Check timers status
	if(timerSwapBottomLayer == 0) {
		timerSwapBottomLayer = app_timer_send_event(g_app_context, SWAP_BOTTOM_LAYER_INTERVAL, 4);
	}

	if(timerUpdateGps == 0) {
		timerUpdateGps = app_timer_send_event(g_app_context, GPS_UPDATE_INTERVAL, 6);
	}
}

void handle_deinit(AppContextRef ctx) {
  (void)ctx;

	for (int i=0; i<NUM_WEATHER_IMAGES; i++) {
	  	heap_bitmap_deinit(&weather_status_imgs[i]);
	  	heap_bitmap_deinit(&weather_status_small_imgs[i]);
	}

  	heap_bitmap_deinit(&bg_image);

	
}


void handle_timer(AppContextRef ctx, AppTimerHandle handle, uint32_t cookie) {
  (void)ctx;
  (void)handle;

/* Request new data from the phone once the timers expire */

	if (cookie != 4) {
		text_layer_set_text(&text_status_layer, "Req.");
		connected =0;
	}

	if (cookie == 1) {
		sendCommandInt(SM_SCREEN_EXIT_KEY, STATUS_SCREEN_APP);
		sendCommandInt(SM_SCREEN_ENTER_KEY, WEATHER_APP);
		sendCommand(SM_STATUS_UPD_WEATHER_KEY);	
		sendCommandInt(SM_SCREEN_EXIT_KEY, WEATHER_APP);
		sendCommandInt(SM_SCREEN_ENTER_KEY, STATUS_SCREEN_APP);
	}

	if (cookie == 2) {
		sendCommand(SM_STATUS_UPD_CAL_KEY);	
	}

	if (cookie == 3) {
		sendCommand(SM_SONG_LENGTH_KEY);	
	}

	if (cookie == 4) {
		app_timer_cancel_event(g_app_context, timerSwapBottomLayer);
		timerSwapBottomLayer = 0;
		
		swap_bottom_layer();	

		timerSwapBottomLayer = app_timer_send_event(g_app_context, SWAP_BOTTOM_LAYER_INTERVAL, 4);
	}
	
	if (cookie == 5) {
		sendCommandInt(SM_SCREEN_ENTER_KEY, WEATHER_APP);
		sendCommand(SM_STATUS_UPD_WEATHER_KEY);	
		sendCommandInt(SM_SCREEN_ENTER_KEY, STATUS_SCREEN_APP);
	}
		
	if (cookie == 6) {
		app_timer_cancel_event(g_app_context, timerUpdateGps);
		timerUpdateGps = 0;

		sendCommandInt(SM_SCREEN_ENTER_KEY, GPS_APP);
		psleep(10);
		sendCommandInt(SM_SCREEN_ENTER_KEY, STATUS_SCREEN_APP);

		timerUpdateGps = app_timer_send_event(g_app_context, GPS_UPDATE_INTERVAL, 6);
	}
}

void pbl_main(void *params) {

  PebbleAppHandlers handlers = {
    .init_handler = &handle_init,
    .deinit_handler = &handle_deinit,
	.messaging_info = {
		.buffer_sizes = {
			.inbound = 124,
			.outbound = 256
		},
		.default_callbacks.callbacks = {
			.in_received = rcv,
			.in_dropped = dropped
		}
	},
	.tick_info = {
	  .tick_handler = &handle_minute_tick,
	  .tick_units = MINUTE_UNIT
	},
    .timer_handler = &handle_timer,

  };
  app_event_loop(params, &handlers);
}