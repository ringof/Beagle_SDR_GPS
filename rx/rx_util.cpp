/*
--------------------------------------------------------------------------------
This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.
This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Library General Public License for more details.
You should have received a copy of the GNU Library General Public
License along with this library; if not, write to the
Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
Boston, MA  02110-1301, USA.
--------------------------------------------------------------------------------
*/

// Copyright (c) 2014-2016 John Seamons, ZL/KF6VO

#include "types.h"
#include "config.h"
#include "kiwi.h"
#include "rx.h"
#include "misc.h"
#include "str.h"
#include "printf.h"
#include "timer.h"
#include "web.h"
#include "peri.h"
#include "spi.h"
#include "gps.h"
#include "cfg.h"
#include "coroutines.h"
#include "net.h"
#include "clk.h"
#include "wspr.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <math.h>
#include <signal.h>
#include <fftw3.h>

// copy admin-related configuration from kiwi.json to new admin.json file
void cfg_adm_transition()
{
	int i;
	bool b;
	const char *s;

	s = cfg_string("user_password", NULL, CFG_REQUIRED);
	admcfg_set_string("user_password", s);
	cfg_string_free(s);
	b = cfg_bool("user_auto_login", NULL, CFG_REQUIRED);
	admcfg_set_bool("user_auto_login", b);
	s = cfg_string("admin_password", NULL, CFG_REQUIRED);
	admcfg_set_string("admin_password", s);
	cfg_string_free(s);
	b = cfg_bool("admin_auto_login", NULL, CFG_REQUIRED);
	admcfg_set_bool("admin_auto_login", b);
	
	i = cfg_int("port", NULL, CFG_REQUIRED);
	admcfg_set_int("port", i);
	
	b = cfg_bool("enable_gps", NULL, CFG_REQUIRED);
	admcfg_set_bool("enable_gps", b);

	b = cfg_bool("update_check", NULL, CFG_REQUIRED);
	admcfg_set_bool("update_check", b);
	b = cfg_bool("update_install", NULL, CFG_REQUIRED);
	admcfg_set_bool("update_install", b);
	
	b = cfg_bool("sdr_hu_register", NULL, CFG_REQUIRED);
	admcfg_set_bool("sdr_hu_register", b);
	s = cfg_string("api_key", NULL, CFG_REQUIRED);
	admcfg_set_string("api_key", s);
	cfg_string_free(s);


	// remove from kiwi.json file
	cfg_rem_string("user_password");
	cfg_rem_bool("user_auto_login");
	cfg_rem_string("admin_password");
	cfg_rem_bool("admin_auto_login");

	cfg_rem_int("port");

	cfg_rem_bool("enable_gps");

	cfg_rem_bool("update_check");
	cfg_rem_bool("update_install");

	cfg_rem_bool("sdr_hu_register");
	cfg_rem_string("api_key");


	// won't be present first time after upgrading from v1.2
	// first admin page connection will create
	if ((s = cfg_object("ip_address", NULL, CFG_OPTIONAL)) != NULL) {
		admcfg_set_object("ip_address", s);
		cfg_object_free(s);
		cfg_rem_object("ip_address");
	}


	// update JSON files
	admcfg_save_json(cfg_adm.json);
	cfg_save_json(cfg_cfg.json);
}

int inactivity_timeout_mins, ip_limit_mins;
int S_meter_cal;
double ui_srate, freq_offset;
int sdr_hu_lo_kHz, sdr_hu_hi_kHz;

#define DC_OFFSET_DEFAULT -0.02
#define DC_OFFSET_DEFAULT_PREV 0.05
#define DC_OFFSET_DEFAULT_20kHz -0.034
TYPEREAL DC_offset_I, DC_offset_Q;

#define WATERFALL_CALIBRATION_DEFAULT -13
#define SMETER_CALIBRATION_DEFAULT -13

void update_vars_from_config()
{
	bool update_cfg = false;
	bool update_admcfg = false;
    bool err;

    // When called by "SET save_cfg/save_adm=":
	//  Makes C copies of vars that must be updated when configuration saved from js.
	//
	// When called by rx_server_init():
	//  Makes C copies of vars that must be updated when configuration loaded from cfg files.
	//  Creates configuration parameters with default values that must exist for client connections.

    inactivity_timeout_mins = cfg_default_int("inactivity_timeout_mins", 0, &update_cfg);
    ip_limit_mins = cfg_default_int("ip_limit_mins", 0, &update_cfg);

    int srate_idx = cfg_default_int("max_freq", 0, &update_cfg);
	ui_srate = srate_idx? 32*MHz : 30*MHz;


    // force DC offsets to the default value if not configured
    // also if set to the previous default value
    int firmware_sel = admcfg_default_int("firmware_sel", 0, &update_admcfg);   // needed below
    int mode_20kHz = (firmware_sel == RX_3_WF_3)? 1:0;
    TYPEREAL Ioff, Ioff_20kHz, Qoff, Qoff_20kHz;
    //printf("mode_20kHz=%d\n", mode_20kHz);

    Ioff = cfg_float("DC_offset_I", &err, CFG_OPTIONAL);
    if (err || Ioff == DC_OFFSET_DEFAULT_PREV) {
        Ioff = DC_OFFSET_DEFAULT;
        cfg_set_float("DC_offset_I", Ioff);
        lprintf("DC_offset_I: no cfg or prev default, setting to default value\n");
        update_cfg = true;
    }

    Qoff = cfg_float("DC_offset_Q", &err, CFG_OPTIONAL);
    if (err || Qoff == DC_OFFSET_DEFAULT_PREV) {
        Qoff = DC_OFFSET_DEFAULT;
        cfg_set_float("DC_offset_Q", Ioff);
        lprintf("DC_offset_Q: no cfg or prev default, setting to default value\n");
        update_cfg = true;
    }

    Ioff_20kHz = cfg_float("DC_offset_20kHz_I", &err, CFG_OPTIONAL);
    if (err) {
        Ioff_20kHz = DC_OFFSET_DEFAULT_20kHz;
        cfg_set_float("DC_offset_20kHz_I", Ioff_20kHz);
        lprintf("DC_offset_20kHz_I: no cfg or prev default, setting to default value\n");
        update_cfg = true;
    }

    Qoff_20kHz = cfg_float("DC_offset_20kHz_Q", &err, CFG_OPTIONAL);
    if (err) {
        Qoff_20kHz = DC_OFFSET_DEFAULT_20kHz;
        cfg_set_float("DC_offset_20kHz_Q", Qoff_20kHz);
        lprintf("DC_offset_20kHz_Q: no cfg or prev default, setting to default value\n");
        update_cfg = true;
    }

    DC_offset_I = mode_20kHz? Ioff_20kHz : Ioff;
    DC_offset_Q = mode_20kHz? Qoff_20kHz : Qoff;
    static bool dc_off_msg;
    if (!dc_off_msg) {
        lprintf("using DC_offsets: I %.6f Q %.6f\n", DC_offset_I, DC_offset_Q);
        dc_off_msg = true;
    }


    S_meter_cal = cfg_default_int("S_meter_cal", SMETER_CALIBRATION_DEFAULT, &update_cfg);
    cfg_default_int("waterfall_cal", WATERFALL_CALIBRATION_DEFAULT, &update_cfg);
    cfg_default_bool("contact_admin", true, &update_cfg);
    cfg_default_int("chan_no_pwd", 0, &update_cfg);
    cfg_default_string("owner_info", "", &update_cfg);
    cfg_default_int("clk_adj", 0, &update_cfg);
    freq_offset = cfg_default_float("freq_offset", 0, &update_cfg);
    sdr_hu_lo_kHz = cfg_default_int("sdr_hu_lo_kHz", 0, &update_cfg);
    sdr_hu_hi_kHz = cfg_default_int("sdr_hu_hi_kHz", 30000, &update_cfg);
    cfg_default_bool("index_html_params.RX_PHOTO_LEFT_MARGIN", true, &update_cfg);
    cfg_default_string("index_html_params.HTML_HEAD", "", &update_cfg);
    cfg_default_string("tlimit_exempt_pwd", "", &update_cfg);
    cfg_default_bool("ext_ADC_clk", false, &update_cfg);
    cfg_default_int("ext_ADC_freq", (int) round(ADC_CLOCK_TYP), &update_cfg);
    cfg_default_bool("ADC_clk_corr", true, &update_cfg);
    cfg_default_string("tdoa_id", "", &update_cfg);
    cfg_default_int("tdoa_nchans", -1, &update_cfg);
    cfg_default_bool("no_wf", false, &update_cfg);
    cfg_default_bool("test_webserver_prio", false, &update_cfg);
    cfg_default_bool("test_deadline_update", false, &update_cfg);
    cfg_default_bool("disable_recent_changes", false, &update_cfg);

    if (wspr_update_vars_from_config()) update_cfg = true;

    int espeed = cfg_default_int("ethernet_speed", 0, &update_cfg);
    static int current_espeed;
    if (espeed != current_espeed) {
        printf("ETH0 espeed %d\n", espeed? 10:100);
        non_blocking_cmd_system_child(
            "kiwi.eth", stprintf("ethtool -s eth0 speed %d duplex full", espeed? 10:100), NO_WAIT);
        current_espeed = espeed;
    }
    
    // fix corruption left by v1.131 dotdot bug
    _cfg_int(&cfg_cfg, "WSPR.autorun", &err, CFG_OPTIONAL|CFG_NO_DOT);
    if (!err) {
        _cfg_set_int(&cfg_cfg, "WSPR.autorun", 0, CFG_REMOVE|CFG_NO_DOT, 0);
        _cfg_set_bool(&cfg_cfg, "index_html_params.RX_PHOTO_LEFT_MARGIN", 0, CFG_REMOVE|CFG_NO_DOT, 0);
        printf("removed v1.131 dotdot bug corruption\n");
        update_cfg = true;
    }

    /* int dom_sel = */ cfg_default_int("sdr_hu_dom_sel", DOM_SEL_NAM, &update_cfg);

    // remove old kiwisdr.example.com default
    cfg_default_string("server_url", "", &update_cfg);
    const char *server_url = cfg_string("server_url", NULL, CFG_REQUIRED);
	if (strcmp(server_url, "kiwisdr.example.com") == 0) {
	    cfg_set_string("server_url", "");
	    update_cfg = true;
    }
    // not sure I want to do this yet..
    #if 0
        // Strange problem where cfg.sdr_hu_dom_sel seems to get changed occasionally between modes
        // DOM_SEL_NAM=0 and DOM_SEL_PUB=2. This can result in DOM_SEL_NAM selected but the corresponding
        // domain field blank which has bad consequences (e.g. TDoA host file corrupted).
        // So do some consistency checking here.
        if (dom_sel == DOM_SEL_NAM && (*server_url == '\0' || strcmp(server_url, "kiwisdr.example.com") == 0)) {
            lprintf("### DOM_SEL check: DOM_SEL_NAM but server_url=\"%s\"\n", server_url);
            lprintf("### DOM_SEL check: forcing change to DOM_SEL_PUB\n");
            cfg_set_int("sdr_hu_dom_sel", DOM_SEL_PUB);
            // FIXME: but then server_url needs to be set when pub ip is detected
            update_cfg = true;
        }
	#endif
    cfg_string_free(server_url); server_url = NULL;
    
    
	if (update_cfg)
		cfg_save_json(cfg_cfg.json);


	// same, but for admin config
	// currently just default values that need to exist
	
    admcfg_default_bool("server_enabled", true, &update_admcfg);
    admcfg_default_bool("auto_add_nat", false, &update_admcfg);
    admcfg_default_bool("duc_enable", false, &update_admcfg);
    admcfg_default_string("duc_user", "", &update_admcfg);
    admcfg_default_string("duc_pass", "", &update_admcfg);
    admcfg_default_string("duc_host", "", &update_admcfg);
    admcfg_default_int("duc_update", 3, &update_admcfg);
    admcfg_default_bool("daily_restart", false, &update_admcfg);
    admcfg_default_int("update_restart", 0, &update_admcfg);
    admcfg_default_string("ip_address.dns1", "8.8.8.8", &update_admcfg);
    admcfg_default_string("ip_address.dns2", "8.8.4.4", &update_admcfg);
    admcfg_default_string("url_redirect", "", &update_admcfg);
    admcfg_default_bool("GPS_tstamp", true, &update_admcfg);
    admcfg_default_bool("use_kalman_position_solver", true, &update_admcfg);
    admcfg_default_int("rssi_azel_iq", 0, &update_admcfg);

    admcfg_default_bool("always_acq_gps", false, &update_admcfg);
    gps.include_alert_gps = admcfg_default_bool("include_alert_gps", false, &update_admcfg);
    //real_printf("gps.include_alert_gps=%d\n", gps.include_alert_gps);
    gps.include_E1B = admcfg_default_bool("include_E1B", true, &update_admcfg);
    //real_printf("gps.include_E1B=%d\n", gps.include_E1B);
    admcfg_default_int("survey", 0, &update_admcfg);
    admcfg_default_int("E1B_offset", 4, &update_admcfg);

    gps.acq_Navstar = admcfg_default_bool("acq_Navstar", true, &update_admcfg);
    if (!gps.acq_Navstar) ChanRemove(Navstar);
    gps.acq_QZSS = admcfg_default_bool("acq_QZSS", true, &update_admcfg);
    if (!gps.acq_QZSS) ChanRemove(QZSS);
    gps.QZSS_prio = admcfg_default_bool("QZSS_prio", false, &update_admcfg);
    gps.acq_Galileo = admcfg_default_bool("acq_Galileo", true, &update_admcfg);
    if (!gps.acq_Galileo) ChanRemove(E1B);
    //real_printf("Navstar=%d QZSS=%d Galileo=%d\n", gps.acq_Navstar, gps.acq_QZSS, gps.acq_Galileo);

    // force plot_E1B true because there is no longer an option switch in the admin interface (to make room for new ones)
    bool plot_E1B = admcfg_default_bool("plot_E1B", true, &update_admcfg);
    if (!plot_E1B) {
	    admcfg_set_bool("plot_E1B", true);
        update_admcfg = true;
    }
    
    // FIXME: resolve problem of ip_address.xxx vs ip_address:{xxx} in .json files
    //admcfg_default_bool("ip_address.use_static", false, &update_admcfg);

	if (update_admcfg)
		admcfg_save_json(cfg_adm.json);


    // one-time-per-run initializations
    
    static bool initial_clk_adj;
    if (!initial_clk_adj) {
        int clk_adj = cfg_int("clk_adj", &err, CFG_OPTIONAL);
        if (err == false) {
            printf("INITIAL clk_adj=%d\n", clk_adj);
            if (clk_adj != 0) {
                clock_manual_adj(clk_adj);
            }
        }
        initial_clk_adj = true;
    }
}

// pass result json back to main process via shmem->status_str
static int _geo_task(void *param)
{
	nbcmd_args_t *args = (nbcmd_args_t *) param;
	char *sp = kstr_sp(args->kstr);
    kiwi_strncpy(shmem->status_str, sp, N_SHMEM_STATUS_STR);
    return 0;
}

static bool geoloc_json(conn_t *conn, const char *geo_host_ip_s, const char *country_s, const char *region_s)
{
	char *cmd_p;
	
    asprintf(&cmd_p, "curl -s --ipv4 \"https://%s\" 2>&1", geo_host_ip_s);
    //cprintf(conn, "GEOLOC: <%s>\n", cmd_p);
    
    // NB: don't use non_blocking_cmd() here to prevent audio gliches
    int status = non_blocking_cmd_func_forall("kiwi.geo", cmd_p, _geo_task, 0, POLL_MSEC(1000));
    free(cmd_p);
    int exit_status;
    if (WIFEXITED(status) && (exit_status = WEXITSTATUS(status))) {
        clprintf(conn, "GEOLOC: failed for %s\n", geo_host_ip_s);
        return false;
    }
    //cprintf(conn, "GEOLOC: returned <%s>\n", shmem->status_str);

	cfg_t cfg_geo;
    if (json_init(&cfg_geo, shmem->status_str) == false) {
        clprintf(conn, "GEOLOC: JSON parse failed for %s\n", geo_host_ip_s);
        return false;
    }
    
    char *country_name = (char *) json_string(&cfg_geo, country_s, NULL, CFG_OPTIONAL);
    char *region_name = (char *) json_string(&cfg_geo, region_s, NULL, CFG_OPTIONAL);
    char *city = (char *) json_string(&cfg_geo, "city", NULL, CFG_OPTIONAL);
    
    char *country;
	if (country_name && strcmp(country_name, "United States") == 0 && region_name && *region_name) {
		country = kstr_cat(region_name, ", USA");
	} else {
		country = kstr_cat(country_name, NULL);     // possible that country_name == NULL
	}

	char *geo = NULL;
	if (city && *city) {
		geo = kstr_cat(geo, city);
		geo = kstr_cat(geo, ", ");
	}
    geo = kstr_cat(geo, country);   // NB: country freed here

    clprintf(conn, "GEOLOC: %s <%s>\n", geo_host_ip_s, kstr_sp(geo));
	free(conn->geo);
    conn->geo = strdup(kstr_sp(geo));
    kstr_free(geo);

    json_string_free(&cfg_geo, country_name);
    json_string_free(&cfg_geo, region_name);
    json_string_free(&cfg_geo, city);
    
	json_release(&cfg_geo);
    return true;
}

static void geoloc_task(void *param)
{
	conn_t *conn = (conn_t *) param;
	char *ip = (isLocal_ip(conn->remote_ip) && ddns.pub_valid)? ddns.ip_pub : conn->remote_ip;

    u4_t i = timer_sec();   // mix it up a bit
    int retry = 0;
    bool okay = false;
    do {
        i = (i+1) % 3;
        if (i == 0) okay = geoloc_json(conn, stprintf("ipapi.co/%s/json", ip), "country_name", "region");
        else
        if (i == 1) okay = geoloc_json(conn, stprintf("extreme-ip-lookup.com/json/%s", ip), "country", "region");
        else
        if (i == 2) okay = geoloc_json(conn, stprintf("get.geojs.io/v1/ip/geo/%s.json", ip), "country", "region");
        retry++;
    } while (!okay && retry < 10);
    if (!okay) clprintf(conn, "GEOLOC: for %s FAILED for all geo servers\n", ip);
}

int current_nusers;
static int last_hour = -1, last_min = -1;

// called periodically (currently every 10 seconds)
void webserver_collect_print_stats(int print)
{
	int i, nusers=0;
	conn_t *c;
	
	// print / log connections
    rx_chan_t *rx;
    for (rx = rx_channels; rx < &rx_channels[rx_chans]; rx++) {
        if (!rx->busy) continue;
		c = rx->conn_snd;
		assert(c != NULL);
		
		u4_t now = timer_sec();
		if (c->freqHz != c->last_freqHz || c->mode != c->last_mode || c->zoom != c->last_zoom) {
			if (print) rx_loguser(c, LOG_UPDATE);
			c->last_tune_time = now;
            c->last_freqHz = c->freqHz;
            c->last_mode = c->mode;
            c->last_zoom = c->zoom;
            c->last_log_time = now;
		} else {
			u4_t diff = now - c->last_log_time;
			if (diff > MINUTES_TO_SEC(5)) {
				if (print) rx_loguser(c, LOG_UPDATE_NC);
			}
			
			//cprintf(c, "TO_MINS=%d exempt=%d\n", inactivity_timeout_mins, c->tlimit_exempt);
			if (!c->inactivity_timeout_override && (inactivity_timeout_mins != 0) && !c->tlimit_exempt) {
			    if (c->last_tune_time == 0) c->last_tune_time = now;    // got here before first set in rx_loguser()
				diff = now - c->last_tune_time;
			    //cprintf(c, "diff=%d now=%d last=%d TO_SECS=%d\n", diff, now, c->last_tune_time,
			    //    MINUTES_TO_SEC(inactivity_timeout_mins));
				if (diff > MINUTES_TO_SEC(inactivity_timeout_mins)) {
                    cprintf(c, "TLIMIT-INACTIVE for %s\n", c->remote_ip);
					send_msg(c, false, "MSG inactivity_timeout=%d", inactivity_timeout_mins);
					c->inactivity_timeout = true;
				}
			}
		}
		
		if (ip_limit_mins && !c->tlimit_exempt) {
		    if (c->tlimit_zombie) {
                // After the browser displays the "time limit reached" error panel the connection
                // hangs open until the watchdog goes off. So have to flag as a zombie to keep the
                // database from getting incorrectly updated.
                //cprintf(c, "TLIMIT-IP zombie %s\n", c->remote_ip);
		    } else {
                int ipl_cur_secs = json_default_int(&cfg_ipl, c->remote_ip, 0, NULL);
                ipl_cur_secs += STATS_INTERVAL_SECS;
                //cprintf(c, "TLIMIT-IP setting database sec:%d for %s\n", ipl_cur_secs, c->remote_ip);
                json_set_int(&cfg_ipl, c->remote_ip, ipl_cur_secs);
                if (ipl_cur_secs >= MINUTES_TO_SEC(ip_limit_mins)) {
                    cprintf(c, "TLIMIT-IP connected LIMIT REACHED cur:%d >= lim:%d for %s\n",
                        SEC_TO_MINUTES(ipl_cur_secs), ip_limit_mins, c->remote_ip);
                    send_msg_encoded(c, "MSG", "ip_limit", "%d,%s", ip_limit_mins, c->remote_ip);
                    c->inactivity_timeout = true;
                    c->tlimit_zombie = true;
                }
            }
		}
		
		// FIXME: disable for now -- causes audio glitches for unknown reasons
		#if 0
		if (!c->geo && !c->try_geoloc && (now - c->arrival) > 10) {
		    clprintf(c, "GEOLOC: %s sent no geoloc info, trying from here\n", c->remote_ip);
		    CreateTask(geoloc_task, (void *) c, SERVICES_PRIORITY);
		    c->try_geoloc = true;
		}
		#endif
		
		nusers++;
	}
	current_nusers = nusers;

	// construct cpu stats response
	int user, sys, idle;
	static int last_user, last_sys, last_idle;
	user = sys = 0;
	u4_t now = timer_ms();
	static u4_t last_now;
	float secs = (float)(now - last_now) / 1000;
	last_now = now;
	
	float del_user = 0;
	float del_sys = 0;
	float del_idle = 0;
	
	char *reply = read_file_string_reply("/proc/stat");
	
	if (reply != NULL) {
		sscanf(kstr_sp(reply), "cpu %d %*d %d %d", &user, &sys, &idle);
		kstr_free(reply);
		//long clk_tick = sysconf(_SC_CLK_TCK);
		del_user = (float)(user - last_user) / secs;
		del_sys = (float)(sys - last_sys) / secs;
		del_idle = (float)(idle - last_idle) / secs;
		//printf("CPU %.1fs u=%.1f%% s=%.1f%% i=%.1f%%\n", secs, del_user, del_sys, del_idle);
		
		// ecpu_use() below can thread block, so cpu_stats_buf must be properly set NULL for reading thread
		if (cpu_stats_buf) {
			char *s = cpu_stats_buf;
			cpu_stats_buf = NULL;
			free(s);
		}
		asprintf(&cpu_stats_buf, "\"ct\":%d,\"cu\":%.0f,\"cs\":%.0f,\"ci\":%.0f,\"ce\":%.0f",
			timer_sec(), del_user, del_sys, del_idle, ecpu_use());
		last_user = user;
		last_sys = sys;
		last_idle = idle;
	}

	// collect network i/o stats
	static const float k = 1.0/1000.0/10.0;		// kbytes/sec every 10 secs
	audio_kbps = audio_bytes*k;
	waterfall_kbps = waterfall_bytes*k;
	
	for (i=0; i <= rx_chans; i++) {
		waterfall_fps[i] = waterfall_frames[i]/10.0;
		waterfall_frames[i] = 0;
	}
	http_kbps = http_bytes*k;
	audio_bytes = waterfall_bytes = http_bytes = 0;

	// on the hour: report number of connected users & schedule updates
	int hour, min; utc_hour_min_sec(&hour, &min, NULL);
	
	if (hour != last_hour) {
		if (print) lprintf("(%d %s)\n", nusers, (nusers==1)? "user":"users");
		last_hour = hour;
	}

	if (min != last_min) {
		schedule_update(hour, min);
		last_min = min;
	}
}
