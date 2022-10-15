/*
 *  AirCast: Chromecast to AirPlay
 *
 *  (c) Philippe, philippe_44@outlook.com
 *
 * See LICENSE
 *
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#include <process.h>
#endif

#include "cross_net.h"
#include "cross_util.h"
#include "cross_thread.h"
#include "cross_log.h"
#include "cross_ssl.h"

#include "aircast.h"
#include "metadata.h"
#include "cast_util.h"
#include "cast_parse.h"
#include "castitf.h"
#include "mdnssd.h"
#include "tinysvcmdns.h"
#include "config_cast.h"
#include "ixml.h"

#define VERSION "v0.3.00.0"" ("__DATE__" @ "__TIME__")"

#define DISCOVERY_TIME 	20
#define MEDIA_VOLUME	0.5

/*----------------------------------------------------------------------------*/
/* globals */
/*----------------------------------------------------------------------------*/
struct sMR	*glMRDevices;
uint16_t	glPortBase, glPortRange;
int32_t		glLogLimit = -1;
int			glMaxDevices = 32;
char		glBinding[16] = "?";

log_level	main_loglevel = lINFO;
log_level	raop_loglevel = lINFO;
log_level	util_loglevel = lWARN;
log_level	cast_loglevel = lINFO;

tMRConfig			glMRConfig = {
							true,	// enabled
							false,	// stop_receiver
							"",		// name
							"flc",	// use_flac
							true,	// metadata
							true,	// flush
							MEDIA_VOLUME,	// media volume (0..1)
							{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
							"",		// rtp/http_latency (0 = use client's request)
							false,	// drift
							"", 	// artwork
					};



/*----------------------------------------------------------------------------*/
/* consts or pseudo-const*/
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
static log_level 			*loglevel = &main_loglevel;
#if LINUX || FREEBSD || SUNOS
static bool					glDaemonize = false;
#endif
static bool					glMainRunning = true;
static struct mDNShandle_s	*glmDNSsearchHandle;
static struct in_addr 		glHost;
static pthread_t 			glMainThread, glmDNSsearchThread;
static char					*glLogFile;
static char					*glHostName = NULL;
static bool					glDiscovery = false;
static bool					glInteractive = true;
static char					*glPidFile = NULL;
static bool					glAutoSaveConfigFile = false;
static bool					glGracefullShutdown = true;
static void					*glConfigID = NULL;
static char					glConfigName[STR_LEN] = "./config.xml";
static struct mdnsd*		glmDNSServer = NULL;

static char usage[] =

			VERSION "\n"
		   "See -t for license terms\n"
		   "Usage: [options]\n"
		   "  -b <ip>\t\tnetwork address to bind to\n"
		   "  -a <port>[:<count>]\tset inbound port and range for RTP and HTTP\n"
		   "  -c <mp3[:<rate>]|flc[:0..9]|wav>\taudio format send to player\n"
   		   "  -v <0..1>\t\t group MediaVolume factor\n"
		   "  -x <config file>\tread config from file (default is ./config.xml)\n"
		   "  -i <config file>\tdiscover players, save <config file> and exit\n"
		   "  -I \t\t\tauto save config at every network scan\n"
		   "  -l <[rtp][:http][:f]>\tRTP and HTTP latency (ms), ':f' forces silence fill\n"
		   "  -r \t\t\tlet timing reference drift (no click)\n"
		   "  -f <logfile>\t\tWrite debug to logfile\n"
		   "  -p <pid file>\t\twrite PID in file\n"
		   "  -d <log>=<level>\tSet logging level, logs: all|raop|main|util|cast, level: error|warn|info|debug|sdebug\n"
#if LINUX || FREEBSD
		   "  -z \t\t\tDaemonize\n"
#endif
		   "  -Z \t\t\tNOT interactive\n"
		   "  -k \t\t\tImmediate exit on SIGQUIT and SIGTERM\n"
		   "  -t \t\t\tLicense terms\n"
   		   "  --noflush\t\tignore flush command (wait for teardown to stop)\n"
		   "\n"
		   "Build options:"
#if LINUX
		   " LINUX"
#endif
#if WIN
		   " WIN"
#endif
#if OSX
		   " OSX"
#endif
#if FREEBSD
		   " FREEBSD"
#endif
#if EVENTFD
		   " EVENTFD"
#endif
#if SELFPIPE
		   " SELFPIPE"
#endif
#if WINEVENT
		   " WINEVENT"
#endif
		   "\n\n";

static char license[] =
		   "This program is free software: you can redistribute it and/or modify\n"
		   "it under the terms of the GNU General Public License as published by\n"
		   "the Free Software Foundation, either version 3 of the License, or\n"
		   "(at your option) any later version.\n\n"
		   "This program is distributed in the hope that it will be useful,\n"
		   "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
		   "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
		   "GNU General Public License for more details.\n\n"
		   "You should have received a copy of the GNU General Public License\n"
		   "along with this program.  If not, see <http://www.gnu.org/licenses/>.\n\n"
	;


/*----------------------------------------------------------------------------*/
/* prototypes */
/*----------------------------------------------------------------------------*/
static void *MRThread(void *args);
static bool  AddCastDevice(struct sMR *Device, char *Name, char *UDN, bool Group, struct in_addr ip, uint16_t port);
static void  RemoveCastDevice(struct sMR *Device);
static bool	 Start(bool cold);
static bool	 Stop(bool exit);

void raop_cb(void *owner, raopsr_event_t event, void *param)
{
	struct sMR *Device = (struct sMR*) owner;

	pthread_mutex_lock(&Device->Mutex);

	// this is async, so player might have been deleted
	if (!Device->Running) {
		LOG_WARN("[%p]: device has been removed", owner);
		pthread_mutex_unlock(&Device->Mutex);
		return;
	}

	switch (event) {
		case RAOP_STREAM:
			// a PLAY will come later, so we'll do the load at that time
			LOG_INFO("[%p]: Stream", Device);
			Device->RaopState = event;
			break;
		case RAOP_STOP:
			LOG_INFO("[%p]: Stop", Device);
			if (Device->RaopState == RAOP_PLAY) {
				CastStop(Device->CastCtx);
				Device->ExpectStop = true;
			}
			Device->RaopState = event;
			break;
		case RAOP_FLUSH:
			if (Device->Config.Flush) {
				LOG_INFO("[%p]: Flush", Device);
				CastStop(Device->CastCtx);
				Device->ExpectStop = true;
				Device->RaopState = event;
			}
			break;
		case RAOP_PLAY: {
			metadata_t MetaData = { .title = "Streaming from AirConnect", .duration = 0, .track = 0 };
			if (*Device->Config.ArtWork) MetaData.artwork = Device->Config.ArtWork;

			LOG_INFO("[%p]: Play", Device);
			if (Device->RaopState != RAOP_PLAY) {
				static int count;
				char *uri, *ContentType;

				(void)!asprintf(&uri, "http://%s:%u/stream-%u", inet_ntoa(glHost), *((short unsigned*) param), count++);
				if (!strcasecmp(Device->Config.Codec, "mp3")) ContentType = "audio/mpeg";
				else if (!strcasecmp(Device->Config.Codec, "wav")) ContentType = "audio/wav";
				else ContentType = "audio/flac";
				CastLoad(Device->CastCtx, uri, ContentType, &MetaData);
				free(uri);
			}

			CastPlay(Device->CastCtx);

			CastSetDeviceVolume(Device->CastCtx, Device->Volume, true);
			Device->RaopState = event;
			break;
		}
		case RAOP_VOLUME: {
			uint32_t now = gettime_ms();

			if (now > Device->VolumeStampRx + 1000) {
				Device->Volume = *((double*) param);
				Device->VolumeStampTx = now;
				CastSetDeviceVolume(Device->CastCtx, Device->Volume, false);
				LOG_INFO("[%p]: Volume[0..1] %0.4lf", Device, Device->Volume);
			}
			break;
		}
		default:
			break;
	}

	pthread_mutex_unlock(&Device->Mutex);
}


/*----------------------------------------------------------------------------*/
#define TRACK_POLL  (1000)
#define MAX_ACTION_ERRORS (5)
static void *MRThread(void *args)
{
	int elapsed, wakeTimer = TRACK_POLL;
	unsigned last = gettime_ms();
	struct sMR *p = (struct sMR*) args;
	json_t *data;

	while (p->Running) {
		double Volume = -1;

		// context is valid until this thread ends, no deletion issue
		data = GetTimedEvent(p->CastCtx, wakeTimer);
		elapsed = gettime_ms() - last;

		// need to protect against events from CC threads, not from deletion
		pthread_mutex_lock(&p->Mutex);

		// need to check status there, protected
		if (!p->Running) {
			pthread_mutex_unlock(&p->Mutex);
			break;
		}

		wakeTimer = (p->State != STOPPED) ? TRACK_POLL : TRACK_POLL * 10;
		LOG_SDEBUG("[%p]: Cast thread timer %d %d", p, elapsed, wakeTimer);

		// a message has been received
		if (data) {
			json_t *val = json_object_get(data, "type");
			const char *type = json_string_value(val);
			uint32_t now = gettime_ms();

			// a mediaSessionId has been acquired
			if (type && !strcasecmp(type, "MEDIA_STATUS")) {
				const char *state = GetMediaItem_S(data, 0, "playerState");

				if (state && !strcasecmp(state, "PLAYING") && p->State != PLAYING) {
					LOG_INFO("[%p]: Cast playing", p);
					p->State = PLAYING;
					if (p->RaopState != RAOP_PLAY) raopsr_notify(p->Raop, RAOP_PLAY, NULL);
				}

				if (state && !strcasecmp(state, "PAUSED") && p->State == PLAYING) {
					LOG_INFO("[%p]: Cast pause", p);
					p->State = PAUSED;
					if (p->RaopState == RAOP_PLAY) raopsr_notify(p->Raop, RAOP_PAUSE, NULL);
				}

				if (state && !strcasecmp(state, "IDLE") && p->State != STOPPED) {
					const char *cause = GetMediaItem_S(data, 0, "idleReason");
					if (cause && !p->ExpectStop) {
						LOG_INFO("[%p]: Cast stopped by other remote", p);
						if (p->RaopState == RAOP_PLAY) raopsr_notify(p->Raop, RAOP_STOP, NULL);
						p->ExpectStop = false;
					}
					p->State = STOPPED;
				}
			}

			// check for volume at the receiver level, but only record the change
			if (type && !strcasecmp(type, "RECEIVER_STATUS")) {
				double volume;
				bool muted;

				if (!p->Group && GetMediaVolume(data, 0, &volume, &muted)) {
					if (volume != -1 && !muted && volume != p->Volume) Volume = volume;
				}
			}

			// now apply the volume change if any
			if (Volume != -1 && fabs(Volume - p->Volume) >= 0.01 && now > p->VolumeStampTx + 1000) {
				p->VolumeStampRx = now;
				p->VolumeStampRx = now;
				LOG_INFO("[%p]: Volume local change %0.4lf", p, Volume);
				raopsr_notify(p->Raop, RAOP_VOLUME, &Volume);
				Volume = -1;
			}

			// always set volume done
			Volume = -1;

			// Cast devices has closed the connection
			if (type && !strcasecmp(type, "CLOSE")) {
				LOG_INFO("[%p]: Cast peer closed connection", p);
				if (p->State != STOPPED) raopsr_notify(p->Raop, RAOP_STOP, NULL);
				p->State = STOPPED;
			}

			json_decref(data);
		}

		// get track position & CurrentURI
		p->TrackPoll += elapsed;
		if (p->TrackPoll >= TRACK_POLL) {
			p->TrackPoll = 0;
			if (p->State != STOPPED) CastGetMediaStatus(p->CastCtx);
		}

		pthread_mutex_unlock(&p->Mutex);
		last = gettime_ms();
	}

	list_clear((list_t**)&p->GroupMaster, free);

	return NULL;
}


/*----------------------------------------------------------------------------*/
char *GetmDNSAttribute(txt_attr_t *p, int count, char *name)
{
	for (int j = 0; j < count; j++)
		if (!strcasecmp(p[j].name, name))
			return strdup(p[j].value);

	return NULL;
}


/*----------------------------------------------------------------------------*/
static struct sMR *SearchUDN(char *UDN)
{
	for (int i = 0; i < glMaxDevices; i++) {
		if (glMRDevices[i].Running && !strcmp(glMRDevices[i].UDN, UDN))
			return glMRDevices + i;
	}

	return NULL;
}


/*----------------------------------------------------------------------------*/
static bool isMember(struct in_addr host) {
	for (int i = 0; i < glMaxDevices; i++) {
		 if (glMRDevices[i].Running && GetAddr(glMRDevices[i].CastCtx).s_addr == host.s_addr)
			return true;
	}
	return false;
}


/*----------------------------------------------------------------------------*/
bool mDNSsearchCallback(mDNSservice_t *slist, void *cookie, bool *stop)
{
	struct sMR *Device;
	mDNSservice_t *s;
	int j;

	if (*loglevel == lDEBUG) {
		LOG_DEBUG("----------------- round ------------------", NULL);
		for (s = slist; s && glMainRunning; s = s->next) {
			char *host = strdup(inet_ntoa(s->host));
			LOG_DEBUG("[%s] host %s, srv %s, name %s ", s->expired  ? "EXPIRED" : "ACTIVE",
						host, inet_ntoa(s->addr), s->name);
			free(host);
		}
	}

	/*
	cast groups creation is difficult - as storm of mDNS message is sent during
	master's election and many masters will claim the group then will "retract"
	one by one. The logic below works well if no announce is missed, which is
	not the case under high traffic, so in that case, either the actual master
	is missed and it will be discovered at the next 20s search or some retractions
	are missed and if the group is destroyed right after creation, then it will
	hang around	until the retractations timeout (2mins) - still correct as the
	end result is with the right master and group is ultimately removed, but not
	very user-friendy
	*/

	for (s = slist; s && glMainRunning; s = s->next) {
		char *UDN = NULL, *Name = NULL;
		char *Model;
		bool Group;

		// is the mDNS record usable or announce made on behalf
		if ((UDN = GetmDNSAttribute(s->attr, s->attr_count, "id")) == NULL ||
			(s->host.s_addr != s->addr.s_addr && isMember(s->host))) continue;

		// is that device already here
		if ((Device = SearchUDN(UDN)) != NULL) {
			// a service is being removed
			Device->Remove = s->expired;
			if (s->expired) {
				// groups need to find if the removed service is the master
				if (Device->Group) {
					// there are some other master candidates
					if (Device->GroupMaster->Next) {
						Device->Remove = false;
						// changing the master, so need to update cast params
						if (Device->GroupMaster->Host.s_addr == s->host.s_addr) {
							free(list_pop((list_t**) &Device->GroupMaster));
							UpdateCastDevice(Device->CastCtx, Device->GroupMaster->Host, Device->GroupMaster->Port);
						} else {
							struct sGroupMember *Member = Device->GroupMaster;
							while (Member && (Member->Host.s_addr != s->host.s_addr)) Member = Member->Next;
							if (Member) free(list_remove((list_t*) Member, (list_t**) &Device->GroupMaster));
						}
					}
				}
			// device update - when playing ChromeCast update their TXT records
			} else {
				char *Name = GetmDNSAttribute(s->attr, s->attr_count, "fn");

				// new master in election, update and put it in the queue
				if (Device->Group && Device->GroupMaster->Host.s_addr != s->addr.s_addr) {
					struct sGroupMember *Member = calloc(1, sizeof(struct sGroupMember));
					Member->Host = s->host;
					Member->Port = s->port;
					list_push((list_t*) Member, (list_t**) &Device->GroupMaster);
				}
				
				UpdateCastDevice(Device->CastCtx, s->addr, s->port);
				
				// update Device name if needed
				if (Name && strcmp(Name, Device->Name) && 
					!strncmp(Device->Name, Device->Config.Name, strlen(Device->Name)) &&
					Device->Config.Name[strlen(Device->Config.Name) - 1] == '+') {
					LOG_INFO("[%p]: Device name change %s %s", Device, Name, Device->Name);
					raopsr_update(Device->Raop, Name, "aircast");
					strcpy(Device->Name, Name);
					sprintf(Device->Config.Name, "%s+", Name);
				}
				NFREE(Name);
			}
			NFREE(UDN);
			continue;
		}

		// disconnect of an unknown device
		if (!s->port && !s->addr.s_addr) {
			LOG_ERROR("Unknown device disconnected %s", s->name);
			NFREE(UDN);
			continue;
		}

		// device creation so search a free spot.
		for (j = 0; j < glMaxDevices && glMRDevices[j].Running; j++);

		// no more room !
		if (j == glMaxDevices) {
			LOG_ERROR("Too many Cast devices", NULL);
			NFREE(UDN);
			break;
		}

		Device = glMRDevices + j;

		// if model is a group
		Model = GetmDNSAttribute(s->attr, s->attr_count, "md");
		if (Model && !strcasestr(Model, "Group")) Group = false;
		else Group = true;
		NFREE(Model);


		Name = GetmDNSAttribute(s->attr, s->attr_count, "fn");
		if (!Name) Name = strdup(s->hostname);
		
		if (AddCastDevice(Device, Name, UDN, Group, s->addr, s->port) && !glDiscovery) {
			Device->Raop = raopsr_create(glHost, glmDNSServer, Device->Config.Name,
										"aircast", Device->Config.mac, Device->Config.Codec,
										Device->Config.Metadata, Device->Config.Drift,
										Device->Config.Flush, Device->Config.Latency,
										Device, raop_cb, NULL, glPortBase, glPortRange, -1);
			if (!Device->Raop) {
				LOG_ERROR("[%p]: cannot create RAOP instance (%s)", Device, Device->Config.Name);
				RemoveCastDevice(Device);
			}
		}

		NFREE(UDN);
		NFREE(Name);
	}

	// look for devices to be removed
	for (j = 0; j < glMaxDevices; j++) {
		Device = glMRDevices + j;
		if (Device->Running && Device->Remove && !CastIsConnected(Device->CastCtx)) {
			LOG_INFO("[%p]: removing renderer (%s) %d", Device, Device->Config.Name);
			raopsr_delete(Device->Raop);
			RemoveCastDevice(Device);
		}
	}

	if (glAutoSaveConfigFile || glDiscovery) {
		LOG_DEBUG("Updating configuration %s", glConfigName);
		SaveConfig(glConfigName, glConfigID, false);
	}

	// we have not released the slist
	return false;
}


/*----------------------------------------------------------------------------*/
static void *mDNSsearchThread(void *args)
{
	// launch the query,
	query_mDNS(glmDNSsearchHandle, "_googlecast._tcp.local", 120,
			   glDiscovery ? DISCOVERY_TIME : 0, &mDNSsearchCallback, NULL);
	return NULL;
}


/*----------------------------------------------------------------------------*/
static void *MainThread(void *args)
{
	while (glMainRunning) {

		crossthreads_sleep(30*1000);
		if (!glMainRunning) break;

		if (glLogFile && glLogLimit != - 1) {
			uint32_t size = ftell(stderr);

			if (size > glLogLimit*1024*1024) {
				uint32_t Sum, BufSize = 16384;
				uint8_t *buf = malloc(BufSize);

				FILE *rlog = fopen(glLogFile, "rb");
				FILE *wlog = fopen(glLogFile, "r+b");
				LOG_DEBUG("Resizing log", NULL);
				for (Sum = 0, fseek(rlog, size - (glLogLimit*1024*1024) / 2, SEEK_SET);
					 (BufSize = fread(buf, 1, BufSize, rlog)) != 0;
					 Sum += BufSize, fwrite(buf, 1, BufSize, wlog));

				Sum = fresize(wlog, Sum);
				fclose(wlog);
				fclose(rlog);
				NFREE(buf);
				if (!freopen(glLogFile, "a", stderr)) {
					LOG_ERROR("re-open error while truncating log", NULL);
				}
			}
		}

		// try to detect IP change when auto-detect
		if (strstr(glBinding, "?")) {
			struct in_addr host;
			get_interface(&host);
			if (host.s_addr != INADDR_ANY && host.s_addr != glHost.s_addr) {
				LOG_INFO("IP change detected %s", inet_ntoa(glHost));
				Stop(false);
				glMainRunning = true;
				Start(false);
			}
		}
	}

	return NULL;
}

/*----------------------------------------------------------------------------*/
static bool AddCastDevice(struct sMR *Device, char *Name, char *UDN, bool group, struct in_addr ip, uint16_t port)
{
	// read parameters from default then config file
	memcpy(&Device->Config, &glMRConfig, sizeof(tMRConfig));
	LoadMRConfig(glConfigID, UDN, &Device->Config);
	if (!Device->Config.Enabled) return false;

	// do not zero-out the structure as the mutex must be preserved
	strcpy(Device->UDN, UDN);
	Device->Magic		= MAGIC;
	Device->Running		= true;
	Device->State 		= STOPPED;
	Device->ExpectStop 	= false;
	Device->Volume 		= Device->Elapsed = Device->TrackPoll = 0;
	Device->CastCtx 	= NULL;
	Device->Raop 		= NULL;
	Device->RaopState	= RAOP_STOP;
	Device->Group 		= group;
	Device->Remove		= false;
	Device->VolumeStampRx = Device->VolumeStampTx = gettime_ms() - 2000;

	if (group) {
		Device->GroupMaster	= calloc(1, sizeof(struct sGroupMember));
		Device->GroupMaster->Host = ip;
		Device->GroupMaster->Port = port;
	} else Device->GroupMaster = NULL;

	if (!*Device->Config.Name) sprintf(Device->Config.Name, "%s+", Name);
	strcpy(Device->Name, Name);

	if (!memcmp(Device->Config.mac, "\0\0\0\0\0\0", 6)) {
		uint32_t mac_size = 6;
		if (group || SendARP(ip.s_addr, INADDR_ANY, Device->Config.mac, &mac_size)) {
			*(uint32_t*) (Device->Config.mac + 2) = hash32(Device->UDN);
			LOG_INFO("[%p]: creating MAC", Device);
		}
		memset(Device->Config.mac, 0xcc, 2);
	}

	// virtual players duplicate mac address
	for (int i = 0; i < glMaxDevices; i++) {
		if (glMRDevices[i].Running && Device != glMRDevices + i && !memcmp(&glMRDevices[i].Config.mac, Device->Config.mac, 6)) {
			memset(Device->Config.mac, 0xcc, 2);
			*(uint32_t*) (Device->Config.mac + 2) = hash32(Device->UDN);
			LOG_INFO("[%p]: duplicated mac ... updating", Device);
		}
	}

	LOG_INFO("[%p]: adding renderer (%s) with mac %hx%x", Device, Name, *(uint16_t*) Device->Config.mac, *(uint32_t*) (Device->Config.mac + 2));

	Device->CastCtx = CreateCastDevice(Device, Device->Group, Device->Config.StopReceiver, ip, port, Device->Config.MediaVolume);
	pthread_create(&Device->Thread, NULL, &MRThread, Device);

	return true;
}


/*----------------------------------------------------------------------------*/
void FlushCastDevices(void) {
	for (int i = 0; i < glMaxDevices; i++) {
		struct sMR *p = &glMRDevices[i];
		if (p->Running) {
			raopsr_delete(p->Raop);
			RemoveCastDevice(p);
		 }
	}
}


/*----------------------------------------------------------------------------*/
void RemoveCastDevice(struct sMR *Device) {
	pthread_mutex_lock(&Device->Mutex);
	Device->Running = false;
	pthread_mutex_unlock(&Device->Mutex);

	// device's thread can still be running but this will wake it up and end it
	DeleteCastDevice(Device->CastCtx);

	pthread_join(Device->Thread, NULL);
}

/*----------------------------------------------------------------------------*/
static bool Start(bool cold) {
	char hostname[STR_LEN];

	get_interface(&glHost);
	if (!strstr(glBinding, "?")) glHost.s_addr = inet_addr(glBinding);
	snprintf(hostname, STR_LEN, "%s.local", glHostName);

	LOG_INFO("Binding to %s", inet_ntoa(glHost));

	if (cold) {
		// manually load openSSL symbols to accept multiple versions
		if (!cross_ssl_load()) {
			LOG_ERROR("Cannot load SSL libraries", NULL);
			return false;
		}

		// mutexes must always be valid
		glMRDevices = calloc(glMaxDevices, sizeof(struct sMR));
		for (int i = 0; i < glMaxDevices; i++) pthread_mutex_init(&glMRDevices[i].Mutex, 0);

		// start the main thread
		pthread_create(&glMainThread, NULL, &MainThread, NULL);
	}

	if (glHost.s_addr != INADDR_ANY) {
		// initialize mDNS broadcast
		if ((glmDNSServer = mdnsd_start(glHost)) == NULL) return false;
		mdnsd_set_hostname(glmDNSServer, hostname, glHost);

		// start the mDNS devices discovery thread
		glmDNSsearchHandle = init_mDNS(false, glHost);
		pthread_create(&glmDNSsearchThread, NULL, &mDNSsearchThread, NULL);
	}

	return true;
}

static bool Stop(bool exit) {
	glMainRunning = false;

	if (glHost.s_addr != INADDR_ANY) {
		LOG_DEBUG("terminate search thread ...", NULL);
		// this forces an ongoing search to end
		close_mDNS(glmDNSsearchHandle);
		pthread_join(glmDNSsearchThread, NULL);

		LOG_DEBUG("flush renderers ...", NULL);
		FlushCastDevices();

		// stop broadcasting devices
		mdnsd_stop(glmDNSServer);
	}

	if (exit) {
		LOG_DEBUG("terminate main thread ...", NULL);
		crossthreads_wake();
		pthread_join(glMainThread, NULL);
		for (int i = 0; i < glMaxDevices; i++) pthread_mutex_destroy(&glMRDevices[i].Mutex);

		NFREE(glHostName);
		if (glConfigID) ixmlDocument_free(glConfigID);
		netsock_close();
		cross_ssl_free();
	}

	return true;
}

/*---------------------------------------------------------------------------*/
static void sighandler(int signum) {
	if (!glGracefullShutdown) {
		for (int i = 0; i < glMaxDevices; i++) {
			struct sMR *p = &glMRDevices[i];
			if (p->Running && p->State == PLAYING) CastStop(p->CastCtx);
		}
		LOG_INFO("forced exit", NULL);
		exit(0);
	}

	Stop(true);
	exit(0);
}


/*---------------------------------------------------------------------------*/
bool ParseArgs(int argc, char **argv) {
	char *optarg = NULL;
	int optind = 1;
	char cmdline[256] = "";

	for (int i = 0; i < argc && (strlen(argv[i]) + strlen(cmdline) + 2 < sizeof(cmdline)); i++) {
		strcat(cmdline, argv[i]);
		strcat(cmdline, " ");
	}

	while (optind < argc && strlen(argv[optind]) >= 2 && argv[optind][0] == '-') {
		char *opt = argv[optind] + 1;
		if (strstr("abxdpiflcv", opt) && optind < argc - 1) {
			optarg = argv[optind + 1];
			optind += 2;
		} else if (strstr("tzZIkr", opt) || opt[0] == '-') {
			optarg = NULL;
			optind += 1;
		}
		else {
			printf("%s", usage);
			return false;
		}

		switch (opt[0]) {
		case 'f':
			glLogFile = optarg;
			break;
		case 'v':
			glMRConfig.MediaVolume = atof(optarg);
			break;
		case 'c':
			strcpy(glMRConfig.Codec, optarg);
			break;
		case 'b':
			strcpy(glBinding, optarg);
			break;
		case 'a':
			sscanf(optarg, "%hu:%hu", &glPortBase, &glPortRange);
			break;
		case 'i':
			strcpy(glConfigName, optarg);
			glDiscovery = true;
			break;
		case 'I':
			glAutoSaveConfigFile = true;
			break;
		case 'p':
			glPidFile = optarg;
			break;
		case 'Z':
			glInteractive = false;
			break;
		case 'k':
			glGracefullShutdown = false;
			break;
		case 'r':
			glMRConfig.Drift = true;
			break;
		case 'l':
			strcpy(glMRConfig.Latency, optarg);
			break;
#if LINUX || FREEBSD
		case 'z':
			glDaemonize = true;
			break;
#endif
		case 'd':
			{
				char *l = strtok(optarg, "=");
				char *v = strtok(NULL, "=");
				log_level new = lWARN;
				if (l && v) {
					if (!strcmp(v, "error"))  new = lERROR;
					if (!strcmp(v, "warn"))   new = lWARN;
					if (!strcmp(v, "info"))   new = lINFO;
					if (!strcmp(v, "debug"))  new = lDEBUG;
					if (!strcmp(v, "sdebug")) new = lSDEBUG;
					if (!strcmp(l, "all") || !strcmp(l, "main")) main_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "util")) util_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "cast")) cast_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "raop")) raop_loglevel = new;
				}
				else {
					printf("%s", usage);
					return false;
				}
			}
			break;
		case 't':
			printf("%s", license);
			return false;
		case '-':
			if (!strcmp(opt + 1, "noflush")) glMRConfig.Flush = false;
			break;
		default:
			break;
		}
	}

	return true;
}


/*----------------------------------------------------------------------------*/
/*																			  */
/*----------------------------------------------------------------------------*/
int main(int argc, char *argv[]) {
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
#if defined(SIGQUIT)
	signal(SIGQUIT, sighandler);
#endif
#if defined(SIGHUP)
	signal(SIGHUP, sighandler);
#endif

	netsock_init();

	// first try to find a config file on the command line
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-x")) {
			strcpy(glConfigName, argv[i+1]);
		}
	}

	// load config from xml file
	glConfigID = (void*) LoadConfig(glConfigName, &glMRConfig);

	// potentially overwrite with some cmdline parameters
	if (!ParseArgs(argc, argv)) exit(1);

	// make sure port range is correct
	if (glPortBase && !glPortRange) glPortRange = glMaxDevices*4;

	if (glLogFile) {
		if (!freopen(glLogFile, "a", stderr)) {
			fprintf(stderr, "error opening logfile %s: %s\n", glLogFile, strerror(errno));
		}
	}

	LOG_ERROR("Starting aircast version: %s", VERSION);

	if (strtod("0.30", NULL) != 0.30) {
		LOG_ERROR("Wrong GLIBC version, use -static build", NULL);
		exit(1);
	}

	if (!glConfigID) {
		LOG_WARN("no config file, using defaults", NULL);
	}

	// just do device discovery and exit
	if (glDiscovery) {
		Start(true);
		sleep(DISCOVERY_TIME + 1);
		Stop(true);
		return(0);
	}

#if LINUX || FREEBSD
	if (glDaemonize) {
		if (daemon(1, glLogFile ? 1 : 0)) {
			fprintf(stderr, "error daemonizing: %s\n", strerror(errno));
		}
	}
#endif

	if (glPidFile) {
		FILE *pid_file;
		pid_file = fopen(glPidFile, "wb");
		if (pid_file) {
			fprintf(pid_file, "%d", (int) getpid());
			fclose(pid_file);
		}
		else {
			LOG_ERROR("Cannot open PID file %s", glPidFile);
		}
	}

	if (!Start(true)) {

		LOG_ERROR("Cannot start", NULL);

		exit(1);

	}

	for (char resp[20] = ""; strcmp(resp, "exit");) {
#if LINUX || FREEBSD || SUNOS
		if (!glDaemonize && glInteractive)
			(void)! scanf("%s", resp);
		else
			pause();
#else
		if (glInteractive)
			(void)! scanf("%s", resp);
		else
#if OSX
			pause();
#else
			Sleep(INFINITE);
#endif
#endif
		char level[20];

		if (!strcmp(resp, "maindbg"))	{
			(void)! scanf("%s", level);
			main_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "utildbg"))	{
			(void)! scanf("%s", level);
			util_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "castdbg"))	{
			(void)! scanf("%s", level);
			cast_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "raopdbg"))	{
			(void)! scanf("%s", level);
			raop_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "save"))	{
			char name[128];
			(void)! scanf("%s", name);
			SaveConfig(name, glConfigID, true);
		}

		if (!strcmp(resp, "dump") || !strcmp(resp, "dumpall"))	{
			bool all = !strcmp(resp, "dumpall");

			for (int i = 0; i < glMaxDevices; i++) {
				struct sMR *p = &glMRDevices[i];
				bool Locked = pthread_mutex_trylock(&p->Mutex);

				if (!Locked) pthread_mutex_unlock(&p->Mutex);
				if (!p->Running && !all) continue;
				printf("%20.20s [r:%u] [l:%u] [s:%u]", p->Config.Name, p->Running,
					   Locked, p->State);
				if (p->Group)
					printf(" [m:%p, n:%p]\n", p->GroupMaster,
						   p->GroupMaster ? p->GroupMaster->Next : NULL);
				printf("\n");
			}
		}

	};

	LOG_INFO("stopping Cast devices ...", NULL);
	Stop(true);
	LOG_INFO("all done", NULL);

	return true;
}




