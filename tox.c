#include "main.h"
#include "tox_bootstrap.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <tox/toxav.h>

#define MAX_CALLS 64

static struct {
    _Bool active;
} call[MAX_CALLS];

#define BUF_SIZE (1024 * 1024)

typedef struct {
    uint8_t msg;
    uint16_t param1, param2;
    void *data;
} TOX_MSG;

static TOX_MSG tox_msg;
static volatile _Bool tox_thread_msg;

static ALCdevice *device_out, *device_in;
static ALCcontext *context;
static ALuint source[MAX_CALLS];

static volatile _Bool av_thread_done = 0;

static FILE_T *file_t[256], **file_tend = file_t;

static uint32_t addfile_t(FILE_T *ft)
{
    uint32_t id = (file_tend - file_t);
    *file_tend++ = ft;
    return id;
}

static void fillbuffer(FILE_T *ft)
{
    ft->buffer_bytes = fread(ft->buffer, 1, ft->sendsize, ft->data);
    ft->finish = (feof((FILE*)ft->data) != 0);
}

static void tox_thread_message(Tox *tox, ToxAv *av, uint8_t msg, uint16_t param1, uint16_t param2, void *data);

void tox_postmessage(uint8_t msg, uint16_t param1, uint16_t param2, void *data)
{
    while(tox_thread_msg) {
        yieldcpu();
    }

    tox_msg.msg = msg;
    tox_msg.param1 = param1;
    tox_msg.param2 = param2;
    tox_msg.data = data;

    tox_thread_msg = 1;
}

#include "tox_callbacks.h"

static void callback_file_send_request(Tox *tox, int32_t fid, uint8_t filenumber, uint64_t filesize, uint8_t *filename, uint16_t filename_length, void *userdata)
{
    tox_file_send_control(tox, fid, 1, filenumber, TOX_FILECONTROL_ACCEPT, NULL, 0);

    FILE_T *ft = friend_newincoming(&friend[fid], filenumber);
    ft->total = filesize;
    memcpy(ft->name, filename, filename_length);
    ft->name[filename_length] = 0;
    debug("File Request\n");

    ft->data = fopen(ft->name, "wb");
}

static void callback_file_control(Tox *tox, int32_t fid, uint8_t receive_send, uint8_t filenumber, uint8_t control, uint8_t *data, uint16_t length, void *userdata)
{
    FILE_T *ft = (receive_send) ? &friend[fid].outgoing[filenumber] : &friend[fid].incoming[filenumber];

    switch(control) {
    case TOX_FILECONTROL_ACCEPT: {
        if(receive_send)
        {
            ft->status = FT_SEND;
            debug("FileAccepted\n");
        }
        break;
    }

    case TOX_FILECONTROL_KILL: {
        ft->status = FT_KILL;
        break;
    }

    case TOX_FILECONTROL_PAUSE: {
        if(ft->status == FT_RECV) {
            ft->status = FT_RECV_PAUSED;
        } else if(ft->status == FT_SEND) {
            ft->status = FT_SEND_PAUSED;
        }
        break;
    }

    case TOX_FILECONTROL_FINISHED: {
        if(!receive_send)
        {
            ft->status = FT_FINISHED;
            fclose(ft->data);
            debug("finsih!\n");
        }
        break;
    }
    }
    debug("File Control\n");
}

static void callback_file_data(Tox *tox, int32_t fid, uint8_t filenumber, uint8_t *data, uint16_t length, void *userdata)
{
    debug("data: %u\n", length);

    FILE_T *ft = &friend[fid].incoming[filenumber];
    fwrite(data, 1, length, ft->data);
}

#include "tox_av.h"

/* bootstrap to dht with bootstrap_nodes */
static void do_bootstrap(Tox *tox)
{
    int i = 0;
    while(i < countof(bootstrap_nodes)) {
        struct bootstrap_node *d = &bootstrap_nodes[i++];
        tox_bootstrap_from_address(tox, d->address, 0, d->port, d->key);
    }
}

static void set_callbacks(Tox *tox)
{
    tox_callback_friend_request(tox, callback_friend_request, NULL);
    tox_callback_friend_message(tox, callback_friend_message, NULL);
    tox_callback_friend_action(tox, callback_friend_action, NULL);
    tox_callback_name_change(tox, callback_name_change, NULL);
    tox_callback_status_message(tox, callback_status_message, NULL);
    tox_callback_user_status(tox, callback_user_status, NULL);
    tox_callback_typing_change(tox, callback_typing_change, NULL);
    tox_callback_read_receipt(tox, callback_read_receipt, NULL);
    tox_callback_connection_status(tox, callback_connection_status, NULL);

    tox_callback_group_invite(tox, callback_group_invite, NULL);
    tox_callback_group_message(tox, callback_group_message, NULL);
    tox_callback_group_action(tox, callback_group_action, NULL);
    tox_callback_group_namelist_change(tox, callback_group_namelist_change, NULL);

    tox_callback_file_send_request(tox, callback_file_send_request, NULL);
    tox_callback_file_control(tox, callback_file_control, NULL);
    tox_callback_file_data(tox, callback_file_data, NULL);
}

static void set_av_callbacks(ToxAv *av)
{
    toxav_register_callstate_callback(callback_av_invite, av_OnInvite, av);
    toxav_register_callstate_callback(callback_av_start, av_OnStart, av);
    toxav_register_callstate_callback(callback_av_cancel, av_OnCancel, av);
    toxav_register_callstate_callback(callback_av_reject, av_OnReject, av);
    toxav_register_callstate_callback(callback_av_end, av_OnEnd, av);
    toxav_register_callstate_callback(callback_av_ringing, av_OnRinging, av);
    toxav_register_callstate_callback(callback_av_starting, av_OnStarting, av);
    toxav_register_callstate_callback(callback_av_ending, av_OnEnding, av);
    toxav_register_callstate_callback(callback_av_error, av_OnError, av);
    toxav_register_callstate_callback(callback_av_requesttimeout, av_OnRequestTimeout, av);
    toxav_register_callstate_callback(callback_av_peertimeout, av_OnPeerTimeout, av);
}

static _Bool load_save(Tox *tox)
{
    uint32_t size;
    void *data = file_raw("tox_save", &size);
    if(!data) {
        return 0;
    }
    int r = tox_load(tox, data, size);
    free(data);

    if(r != 0) {
        return 0;
    }

    friends = tox_count_friendlist(tox);

    uint32_t i = 0;
    while(i != friends) {
        int size;
        FRIEND *f = &friend[i];
        uint8_t name[128];

        tox_get_client_id(tox, i, f->cid);

        size = tox_get_name(tox, i, name);

        friend_setname(f, name, size);

        size = tox_get_status_message_size(tox, i);
        f->status_message = malloc(size);
        tox_get_status_message(tox, i, f->status_message, size);
        f->status_length = size;

        i++;
    }

    self.name_length = tox_get_self_name(tox, self.name);
    self.statusmsg_length = tox_get_self_status_message_size(tox);
    self.statusmsg = malloc(self.statusmsg_length);
    tox_get_self_status_message(tox, self.statusmsg, self.statusmsg_length);
    self.status = tox_get_self_user_status(tox);


    return 1;
}

static void load_defaults(Tox *tox)
{
    uint8_t *name = (uint8_t*)DEFAULT_NAME, *status = (uint8_t*)DEFAULT_STATUS;
    uint16_t name_len = sizeof(DEFAULT_NAME) - 1, status_len = sizeof(DEFAULT_STATUS) - 1;

    tox_set_name(tox, name, name_len);
    tox_set_status_message(tox, status, status_len);

    self.name_length = name_len;
    memcpy(self.name, name, name_len);
    self.statusmsg_length = status_len;
    self.statusmsg = malloc(status_len);
    memcpy(self.statusmsg, status, status_len);
}

static void write_save(Tox *tox)
{
    FILE *file;
    void *data;
    uint32_t size;

    size = tox_size(tox);
    data = malloc(size);
    tox_save(tox, data);
    file = fopen("tox_save", "wb");
    if(file) {
        fwrite(data, size, 1, file);
        fclose(file);
        debug("Saved data\n");
    }

    free(data);
}

static void av_thread(void *args)
{
    ToxAv *av = args;
    ALuint *p, *end;
    const char *device_list;
    _Bool record;

    set_av_callbacks(av);

    device_list = alcGetString(NULL, ALC_CAPTURE_DEVICE_SPECIFIER);
    if(device_list) {
        debug("Input Device List:\n");
        while(*device_list) {
            printf("%s\n", device_list);
            device_list += strlen(device_list) + 1;
        }
    }

    device_list = alcGetString(NULL, ALC_ALL_DEVICES_SPECIFIER);
    if(device_list) {
        debug("Output Device List:\n");
        while(*device_list) {
            printf("%s\n", device_list);
            device_list += strlen(device_list) + 1;
        }
    }

    device_out = alcOpenDevice(NULL);
    if(!device_out) {
        printf("alcOpenDevice() failed\n");
        return;
    }

    context = alcCreateContext(device_out, NULL);
    if(!alcMakeContextCurrent(context)) {
        printf("alcMakeContextCurrent() failed\n");
        alcCloseDevice(device_out);
        return;
    }

    device_in = alcCaptureOpenDevice(NULL, av_DefaultSettings.audio_sample_rate, AL_FORMAT_MONO16, (av_DefaultSettings.audio_frame_duration * av_DefaultSettings.audio_sample_rate * 2) / 1000);
    if(!device_in) {
        printf("no audio input, disabling audio input\n");
        record = 0;
    } else {
        alcCaptureStart(device_in);
        record = 1;
    }

    alListener3f(AL_POSITION, 0.0, 0.0, 0.0);
    alListener3f(AL_VELOCITY, 0.0, 0.0, 0.0);

    alGenSources(countof(source), source);
    p = source;
    end = p + countof(source);
    while(p != end) {
        ALuint s = *p++;
        alSourcef(s, AL_PITCH, 1.0);
        alSourcef(s, AL_GAIN, 1.0);
        alSource3f(s, AL_POSITION, 0.0, 0.0, 0.0);
        alSource3f(s, AL_VELOCITY, 0.0, 0.0, 0.0);
        alSourcei(s, AL_LOOPING, AL_FALSE);
    }

    int perframe = (av_DefaultSettings.audio_frame_duration * av_DefaultSettings.audio_sample_rate) / 1000;
    uint8_t buf[perframe * 2], dest[perframe * 2];

    while(tox_thread_run) {
        if(record) {
            ALint samples;
            alcGetIntegerv(device_in, ALC_CAPTURE_SAMPLES, sizeof(samples), &samples);
            if(samples >= perframe) {
                alcCaptureSamples(device_in, buf, perframe);

                int i = 0;
                while(i < MAX_CALLS) {
                    if(call[i].active) {
                        int r;
                        if((r = toxav_prepare_audio_frame(av, i, dest, perframe * 2, (void*)buf, perframe)) < 0) {
                            debug("toxav_prepare_audio_frame error %i\n", r);
                        }

                        if((r = toxav_send_audio(av, i, dest, r)) < 0) {
                            debug("toxav_send_audio error %i %s\n", r, strerror(errno));
                        }
                    }
                    i++;
                }
            }

        }

        int i = 0;
        while(i < MAX_CALLS) {
            if(call[i].active) {
                int size = toxav_recv_audio(av, i, perframe, (void*)buf);
                if(size > 0) {
                    ALuint bufid;
                    ALint processed;
                    alGetSourcei(source[i], AL_BUFFERS_PROCESSED, &processed);

                    if(processed) {
                        alSourceUnqueueBuffers(source[i], 1, &bufid);
                    } else {
                        alGenBuffers(1, &bufid);
                    }

                    alBufferData(bufid, AL_FORMAT_MONO16, buf, size * 2, av_DefaultSettings.audio_sample_rate);
                    alSourceQueueBuffers(source[i], 1, &bufid);

                    ALint state;;
                    alGetSourcei(source[i], AL_SOURCE_STATE, &state);
                    if(state != AL_PLAYING) {
                        alSourcePlay(source[i]);
                        debug("Starting source %u\n", i);
                    }
                }
            }
            i++;
        }

        yieldcpu();
    }

    //missing some cleanup

    if(record) {
        alcCaptureStop(device_in);
        alcCaptureCloseDevice(device_in);
    }

    alcCloseDevice(device_out);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(context);

    av_thread_done = 1;
}

void tox_thread(void *args)
{
    Tox *tox;
    ToxAv *av;
    uint8_t id[TOX_FRIEND_ADDRESS_SIZE];

    if((tox = tox_new(IPV6_ENABLED)) == NULL) {
        printf("tox_new() failed\n");
        exit(1);
    }

    if(!load_save(tox)) {
        printf("No save file, using defaults\n");
        load_defaults(tox);
    }

    edit_setstr(&edit_name, self.name, self.name_length);
    edit_setstr(&edit_status, self.statusmsg, self.statusmsg_length);

    tox_get_address(tox, id);
    id_to_string(self.id, id);

    printf("Tox ID: %.*s\n", (int)sizeof(self.id), self.id);

    set_callbacks(tox);

    do_bootstrap(tox);

    av = toxav_new(tox, MAX_CALLS);

    tox_thread_run = 1;
    thread(av_thread, av);

    uint64_t last_save = get_time();
    while(tox_thread_run) {
        tox_do(tox);

        if(tox_isconnected(tox) != tox_connected) {
            tox_connected = !tox_connected;
            postmessage(DHT_CONNECTED, 0, 0, NULL);

            debug("Connected to DHT: %u\n", tox_connected);
        }

        if(get_time() - last_save >= (uint64_t)10 * 1000 * 1000 * 1000) {
            last_save = get_time();

            if(!tox_connected) {
                do_bootstrap(tox);
            }

            write_save(tox);
        }

        if(tox_thread_msg) {
            TOX_MSG *msg = &tox_msg;
            tox_thread_message(tox, av, msg->msg, msg->param1, msg->param2, msg->data);
            tox_thread_msg = 0;
        }

        FILE_T **p = file_t;
        while(p != file_tend) {
            FILE_T *ft = *p;
            switch(ft->status) {
            case FT_SEND: {
                if(ft->status == FT_SEND) {
                    while(tox_file_send_data(tox, ft->fid, ft->filenumber, ft->buffer, ft->buffer_bytes) != -1) {
                        if(ft->finish) {
                            tox_file_send_control(tox, ft->fid, 0, ft->filenumber, TOX_FILECONTROL_FINISHED, NULL, 0);
                            free(ft->buffer);
                            fclose(ft->data);
                            ft->status = FT_FINISHED;
                            break;
                        }

                        fillbuffer(ft);
                    }
                }
                break;
            }
            }
            p++;
        }

        yieldcpu();
    }

    write_save(tox);

    toxav_kill(av);
    tox_kill(tox);

    while(!av_thread_done) {
        yieldcpu();
    }

    tox_thread_run = 1;
}

static void tox_thread_message(Tox *tox, ToxAv *av, uint8_t msg, uint16_t param1, uint16_t param2, void *data)
{
    switch(msg) {
    case TOX_SETNAME: {
        /* param1: name length
         * data: name
         */
        tox_set_name(tox, data, param1);
        break;
    }

    case TOX_SETSTATUSMSG: {
        /* param1: status length
         * data: status message
         */
        tox_set_status_message(tox, data, param1);
        break;
    }

    case TOX_SETSTATUS: {
        /* param1: status
         */
        tox_set_user_status(tox, param1);
        break;
    }

    case TOX_ADDFRIEND: {
        /* param1: length of message
         * data: friend id + message
         */
        int r = tox_add_friend(tox, data, data + TOX_FRIEND_ADDRESS_SIZE, param1);
        postmessage(FRIEND_ADD, (r < 0), (r < 0) ? ~r : r, data);
        break;
    }

    case TOX_DELFRIEND: {
        /* param1: friend #
         */
        tox_del_friend(tox, param1);
        break;
    }

    case TOX_ACCEPTFRIEND: {
        /* data: FRIENDREQ
         */
        FRIENDREQ *req = data;
        int r = tox_add_friend_norequest(tox, req->id);
        postmessage(FRIEND_ACCEPT, (r < 0), (r < 0) ? 0 : r, req);
        break;
    }

    case TOX_SENDMESSAGE: {
        /* param1: friend #
         * param2: message length
         * data: message
         */
        tox_send_message(tox, param1, data, param2);
        free(data);
        break;
    }

    case TOX_SENDMESSAGEGROUP: {
        /* param1: group #
         * param2: message length
         * data: message
         */
        tox_group_message_send(tox, param1, data, param2);
        free(data);
        break;
    }

    case TOX_CALL: {
        /* param1: friend #
         */
        int32_t id;
        toxav_call(av, &id, param1, TypeAudio, 10);

        postmessage(FRIEND_CALL_RING, param1, id, NULL);
        break;
    }

    case TOX_ACCEPTCALL: {
        /* param1: call #
         */
        toxav_answer(av, param1, TypeAudio);
        break;
    }

    case TOX_HANGUP: {
        /* param1: call #
         */
        toxav_hangup(av, param1);
        break;
    }

    case TOX_NEWGROUP: {
        /*
         */
        int g = tox_add_groupchat(tox);
        if(g != -1) {
            postmessage(GROUP_ADD, g, 0, NULL);
        }

        break;
    }

    case TOX_LEAVEGROUP: {
        /* param1: group #
         */
        tox_del_groupchat(tox, param1);
        break;
    }

    case TOX_GROUPINVITE: {
        /* param1: group #
         * param2: friend #
         */
        tox_invite_friend(tox, param2, param1);
        break;
    }

    case TOX_SENDFILE: {
        /* param1: friend #
         * param2: file namelength
         * data: length (8 bytes) + name
         */
        uint64_t size = *(uint64_t*)data;
        int filenumber = tox_new_file_sender(tox, param1, size, data + 8, param2);
        if(filenumber != -1) {
            FILE_T *ft = friend_newoutgoing(&friend[param1], filenumber);
            if(ft) {
                uint32_t id = addfile_t(ft);

                ft->fid = param1;
                ft->filenumber = filenumber;

                ft->status = FT_SEND_PENDING;
                ft->sendsize = tox_file_data_size(tox, param1);

                memcpy(ft->name, data + 8, param2);
                memset(ft->name + param2, 0, 1);
                ft->total = size;

                ft->data = fopen(ft->name, "rb");
                ft->buffer = malloc(ft->sendsize);
                fillbuffer(ft);
            }
        }

        free(data);

        break;
    }
    }
}

void tox_message(uint8_t msg, uint16_t param1, uint16_t param2, void *data)
{
    switch(msg) {
    case DHT_CONNECTED: {
        redraw();
        break;
    }

    case FRIEND_REQUEST: {
        /* received a friend request */
        list_addfriendreq(data);
        break;
    }

    case FRIEND_ADD: {
        /* confirmation that friend has been added to friend list (add) */
        if(param1) {
            /* friend was not added */
            addfriend_status = param2 + 3;
            redraw();//ui_drawmain();
        } else {
            /* friend was added */
            edit_addid.length = 0;
            edit_addmsg.length = 0;
            //edit_draw(&edit_addid);
            //edit_draw(&edit_addmsg);

            FRIEND *f = &friend[param2];
            friends++;

            memcpy(f->cid, data, sizeof(f->cid));

            friend_setname(f, NULL, 0);

            //list_addfriend(f);

            addfriend_status = 1;
            //ui_drawmain();
            redraw();
        }

        free(data);
        break;
    }

    case FRIEND_ACCEPT: {
        /* confirmation that friend has been added to friend list (accept) */
        if(!param1) {
            FRIEND *f = &friend[param2];
            FRIENDREQ *req = data;
            friends++;

            memcpy(f->cid, req->id, sizeof(f->cid));
            friend_setname(f, NULL, 0);

            list_addfriend2(f, req);
        }

        free(data);
        break;
    }

    case FRIEND_MESSAGE: {
        FRIEND *f = &friend[param1];

        f->message = realloc(f->message, (f->msg + 1) * sizeof(void*));
        f->message[f->msg++] = data;

        if(sitem && f == sitem->data) {
            redraw();//ui_drawmain();
        }
        break;
    }

#define updatefriend(fp) redraw();//list_draw(); if(sitem && fp == sitem->data) {ui_drawmain();}
#define updategroup(gp) redraw();//list_draw(); if(sitem && gp == sitem->data) {ui_drawmain();}

    case FRIEND_NAME: {
        FRIEND *f = &friend[param1];
        friend_setname(f, data, param2);
        updatefriend(f);
        break;
    }

    case FRIEND_STATUS_MESSAGE: {
        FRIEND *f = &friend[param1];
        free(f->status_message);
        f->status_length = param2;
        f->status_message = data;
        updatefriend(f);
        break;
    }

    case FRIEND_STATUS: {
        FRIEND *f = &friend[param1];
        f->status = param2;
        updatefriend(f);
        break;
    }

    case FRIEND_TYPING: {
        FRIEND *f = &friend[param1];
        f->typing = param2;
        updatefriend(f);
        break;
    }

    case FRIEND_ONLINE: {
        FRIEND *f = &friend[param1];
        f->online = param2;
        updatefriend(f);
        break;
    }

    case FRIEND_CALL_INVITE: {
        FRIEND *f = &friend[param1];
        if(f->calling) {
            //reject call
        } else {
            f->calling = 1;
            f->callid = param2;
            updatefriend(f);
        }
        break;
    }

    case FRIEND_CALL_RING: {
        FRIEND *f = &friend[param1];
        f->calling = 2;
        f->callid = param2;
        updatefriend(f);
        break;
    }

    case FRIEND_CALL_START: {
        FRIEND *f = &friend[param1];
        f->calling = 3;
        updatefriend(f);
        break;
    }

    case FRIEND_CALL_END: {
        FRIEND *f = &friend[param1];
        f->calling = 0;
        updatefriend(f);
        break;
    }

    case GROUP_ADD: {
        GROUPCHAT *g = &group[param1];
        g->name_length = sprintf((char*)g->name, "Groupchat #%u", param1);
        list_addgroup(g);
        break;
    }

    case GROUP_MESSAGE: {
        GROUPCHAT *g = &group[param1];

        g->message = realloc(g->message, (g->msg + 1) * sizeof(void*));
        g->message[g->msg++] = data;

        if(sitem && g == sitem->data) {
            redraw();//ui_drawmain();
        }

        break;
    }

    case GROUP_PEER_DEL: {
        GROUPCHAT *g = &group[param1];

        if(g->peername[param2]) {
            free(g->peername[param2]);
            g->peername[param2] = NULL;
            g->peers--;
        }

        g->topic_length = sprintf((char*)g->topic, "%u users in chat", g->peers);

        updategroup(g);

        break;
    }

    case GROUP_PEER_ADD:
    case GROUP_PEER_NAME: {
        GROUPCHAT *g = &group[param1];

        if(g->peername[param2]) {
            free(g->peername[param2]);
        } else {
            g->peers++;
        }

        if(msg == GROUP_PEER_ADD) {
            uint8_t *n = malloc(10);
            n[0] = 9;
            memcpy(n + 1, "<unknown>", 9);
            data = n;
        }

        g->peername[param2] = data;

        g->topic_length = sprintf((char*)g->topic, "%u users in chat", g->peers);

        if(sitem && g == sitem->data) {
            redraw();//ui_drawmain();
        }

        break;
    }

    case FILE_BEGIN_RECV: {
        //ft->file =
        break;
    }

    case FILE_BEGIN_SEND: {
        //FILE_T *ft = data;
        break;
    }
    }
}