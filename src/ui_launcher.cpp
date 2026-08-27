#include "ui_launcher.h"
#include "display.h"
#include "button_input.h"
#include "settings.h"
#include "emulator_bridge.h"
#include "hw_config.h"
#include <Arduino.h>

// ─── Helpers ────────────────────────────────────────────────────────────────
static void wait_release() { 
	// wait until no button is pressed
	button_update();
	while(button_get_buttons()) { button_update(); delay(10); }
	delay(100);
}

// ─── In-game menu ───────────────────────────────────────────────────────────
static void mbtn(int x, int w, int y, const char* t, uint16_t fg, bool hl) {
	uint16_t bg = hl ? 0x2945 : 0x1082;
	tft.fillRoundRect(x,y,w,26,5,bg);
	tft.drawRoundRect(x,y,w,26,5,0x528A);
	tft.setTextColor(fg,bg); tft.setTextDatum(MC_DATUM);
	tft.drawString(t,SCREEN_W/2,y+13,2);
}

int launcher_ingame_menu() {
	const int panel_w = SCREEN_W - 24;
	const int panel_x = (SCREEN_W - panel_w) / 2;
	const int panel_y = 10;
	const int panel_h = 220;
	const int btn_w = panel_w - 24;
	const int btn_x = panel_x + 12;

	tft.fillRect(panel_x,panel_y,panel_w,panel_h,TFT_BLACK);
	tft.drawRoundRect(panel_x,panel_y,panel_w,panel_h,6,0x528A);
	tft.setTextColor(0xFFE0,TFT_BLACK); tft.setTextDatum(MC_DATUM);
	tft.drawString("PAUSED",SCREEN_W/2,28,4);

	#define MI 5
	int yp[MI]={58,88,118,148,178};
	const char* lb[MI]={"Resume","Save Game","Load Save","Settings","Quit"};
	uint16_t fc[MI]={TFT_GREEN,0x07FF,0x07FF,0xFFE0,TFT_RED};
	for(int i=0;i<MI;i++) mbtn(btn_x,btn_w,yp[i],lb[i],fc[i],false);
	wait_release();

	int hl=0;
	mbtn(btn_x,btn_w,yp[hl],lb[hl],fc[hl],true);
	uint16_t prev = 0;
	while(true) {
		button_update();
		uint16_t b = button_get_buttons();
		if ((b & GB_BTN_UP) && !(prev & GB_BTN_UP)) {
			mbtn(btn_x,btn_w,yp[hl],lb[hl],fc[hl],false);
			hl = (hl==0)?MI-1:hl-1;
			mbtn(btn_x,btn_w,yp[hl],lb[hl],fc[hl],true);
		}
		if ((b & GB_BTN_DOWN) && !(prev & GB_BTN_DOWN)) {
			mbtn(btn_x,btn_w,yp[hl],lb[hl],fc[hl],false);
			hl = (hl+1)%MI;
			mbtn(btn_x,btn_w,yp[hl],lb[hl],fc[hl],true);
		}
		// A = select
		if ((b & GB_BTN_A) && !(prev & GB_BTN_A)) {
			int s = hl;
			// 0=resume 1=save 2=load 3=settings 4=quit
			switch(s){case 0:return 0;case 1:return 1;case 2:return 2;case 3:return 5;case 4:return 3;}
		}
		// B = cancel -> resume
		if ((b & GB_BTN_B) && !(prev & GB_BTN_B)) return 0;

		prev = b;
		delay(15);
	}
}

// ─── Settings menu ──────────────────────────────────────────────────────────
void launcher_settings_menu(settings_t* s) {
	uint8_t pal = s->palette;
	uint8_t fs = s->frameskip;
	uint8_t bl = s->brightness;

	// Selected row: 0=palette,1=frameskip,2=brightness,3=done
	int sel = 0;
	uint16_t prev = 0;

	auto draw_settings = [&](int selrow) {
		const int row_x = 12;
		const int row_w = SCREEN_W - 24;
		const int arrow_l = row_x + 10;
		const int arrow_r = row_x + row_w - 10;

		tft.fillScreen(TFT_BLACK);
		tft.setTextDatum(MC_DATUM);
		tft.setTextColor(0xFFE0); tft.drawString("SETTINGS",SCREEN_W/2,15,4);

		// Palette
		tft.setTextColor(TFT_WHITE); tft.drawString("Color Palette:",SCREEN_W/2,50,2);
		uint16_t palbg = (selrow==0)?0x2945:0x1082;
		tft.fillRoundRect(row_x,65,row_w,28,5,palbg);
		char palstr[40]; snprintf(palstr,40,"%d/%d %s",pal+1,NUM_PALETTES,emu_get_palette_name(pal));
		tft.setTextColor(0x07E0,palbg);
		tft.drawString(palstr,SCREEN_W/2,79,2);
		tft.setTextColor(0x7BEF,palbg);
		tft.setTextDatum(ML_DATUM); tft.drawString("<<",arrow_l,79,2);
		tft.setTextDatum(MR_DATUM); tft.drawString(">>",arrow_r,79,2);

		// Frame skip
		tft.setTextDatum(MC_DATUM);
		tft.setTextColor(TFT_WHITE); tft.drawString("Frame Skip:",SCREEN_W/2,105,2);
		uint16_t fsbg = (selrow==1)?0x2945:0x1082;
		tft.fillRoundRect(row_x,120,row_w,28,5,fsbg);
		char fss[16]; snprintf(fss,16,"%d (FPS ~%d)",fs, fs==0?60:60/(fs+1));
		tft.setTextColor(0x07E0,fsbg); tft.drawString(fss,SCREEN_W/2,134,2);
		tft.setTextColor(0x7BEF,fsbg);
		tft.setTextDatum(ML_DATUM); tft.drawString("<",arrow_l,134,2);
		tft.setTextDatum(MR_DATUM); tft.drawString(">",arrow_r,134,2);

		// Brightness
		tft.setTextDatum(MC_DATUM);
		tft.setTextColor(TFT_WHITE); tft.drawString("Brightness:",SCREEN_W/2,160,2);
		uint16_t blbg = (selrow==2)?0x2945:0x1082;
		tft.fillRoundRect(row_x,175,row_w,28,5,blbg);
		char bls[16]; snprintf(bls,16,"%d%%",bl*100/255);
		tft.setTextColor(0x07E0,blbg); tft.drawString(bls,SCREEN_W/2,189,2);
		tft.setTextColor(0x7BEF,blbg);
		tft.setTextDatum(ML_DATUM); tft.drawString("<",arrow_l,189,2);
		tft.setTextDatum(MR_DATUM); tft.drawString(">",arrow_r,189,2);

		// Done button
		uint16_t donebg = (selrow==3)?0x2945:0x07E0;
		tft.fillRoundRect(100,302,120,16,5,donebg);
		tft.setTextColor(TFT_BLACK,donebg); tft.setTextDatum(MC_DATUM);
		tft.drawString("DONE",SCREEN_W/2,310,1);
	};

	draw_settings(sel);
	wait_release();

	while(true) {
		button_update();
		uint16_t b = button_get_buttons();
		// Navigation: up/down change selected row (edge detect)
		if ((b & GB_BTN_UP) && !(prev & GB_BTN_UP)) { sel = (sel==0)?3:sel-1; draw_settings(sel); }
		if ((b & GB_BTN_DOWN) && !(prev & GB_BTN_DOWN)) { sel = (sel+1)%4; draw_settings(sel); }

		// Row actions (use edge detection where appropriate)
		if (sel==0) {
			if ((b & GB_BTN_LEFT) && !(prev & GB_BTN_LEFT)) { pal = (pal+NUM_PALETTES-1)%NUM_PALETTES; emu_set_palette(pal); draw_settings(sel); }
			if ((b & GB_BTN_RIGHT) && !(prev & GB_BTN_RIGHT)) { pal = (pal+1)%NUM_PALETTES; emu_set_palette(pal); draw_settings(sel); }
		} else if (sel==1) {
			if ((b & GB_BTN_LEFT) && !(prev & GB_BTN_LEFT)) { if (fs>0) { fs--; emu_set_frame_skip(fs); draw_settings(sel); } }
			if ((b & GB_BTN_RIGHT) && !(prev & GB_BTN_RIGHT)) { if (fs<4) { fs++; emu_set_frame_skip(fs); draw_settings(sel); } }
		} else if (sel==2) {
			if ((b & GB_BTN_LEFT) && !(prev & GB_BTN_LEFT)) { if (bl>30) { bl-=25; display_set_backlight(bl); draw_settings(sel); } }
			if ((b & GB_BTN_RIGHT) && !(prev & GB_BTN_RIGHT)) { if (bl<255) { bl=min(255,bl+25); display_set_backlight(bl); draw_settings(sel); } }
		} else if (sel==3) {
			if ((b & GB_BTN_A) && !(prev & GB_BTN_A)) {
				s->palette = pal;
				s->frameskip = fs;
				s->brightness = bl;
				settings_save(s);
				wait_release();
				return;
			}
		}

		// A anywhere acts as confirm for done as well
		if ((b & GB_BTN_A) && !(prev & GB_BTN_A) && sel>=0 && sel<=2) {
			// if not on DONE, treat A as toggle to next row (optional)
			sel = (sel+1)%4; draw_settings(sel);
		}

		prev = b;
		delay(20);
	}
}
