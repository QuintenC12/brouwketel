#ifndef EEZ_LVGL_UI_EVENTS_H
#define EEZ_LVGL_UI_EVENTS_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void action_start_manueel(lv_event_t * e);
extern void action_start_automatisch(lv_event_t * e);
extern void action_open_instellingen(lv_event_t * e);
extern void action_verbinden_annuleren(lv_event_t * e);
extern void action_pomp_toggle(lv_event_t * e);
extern void action_pauze(lv_event_t * e);
extern void action_stop(lv_event_t * e);
extern void action_edit_temp_min(lv_event_t * e);
extern void action_edit_temp_plus(lv_event_t * e);
extern void action_edit_tijd_min(lv_event_t * e);
extern void action_edit_tijd_plus(lv_event_t * e);
extern void action_edit_annuleren(lv_event_t * e);
extern void action_edit_opslaan(lv_event_t * e);
extern void action_stop_annuleren(lv_event_t * e);
extern void action_stop_bevestigen(lv_event_t * e);
extern void action_start_koken(lv_event_t * e);
extern void action_hop_toegevoegd(lv_event_t * e);
extern void action_alarm_stil(lv_event_t * e);
extern void action_sensor_opnieuw(lv_event_t * e);
extern void action_instellingen_terug(lv_event_t * e);
extern void action_instellingen_opslaan(lv_event_t * e);
extern void action_zoemer_toggle(lv_event_t * e);
extern void action_alarm_test(lv_event_t * e);
extern void action_schema_annuleren(lv_event_t * e);
extern void action_schema_start(lv_event_t * e);
extern void action_schema_stap_aanpassen(lv_event_t * e);
extern void action_schema_stap_select(lv_event_t * e);
extern void action_mpause_select(lv_event_t * e);
extern void action_mpause_aanpassen(lv_event_t * e);
extern void action_hervatten(lv_event_t * e);
extern void action_open_webapp(lv_event_t * e);
extern void action_qr_terug(lv_event_t * e);
extern void action_start_action1(lv_event_t * e);
extern void action_numpad_toets(lv_event_t * e);
extern void action_numpad_ok(lv_event_t * e);
extern void action_numpad_annuleren(lv_event_t * e);
extern void action_instelling_numpad(lv_event_t * e);
extern void action_schema_stap_toevoegen(lv_event_t * e);
extern void action_schema_stap_verwijderen(lv_event_t * e);
extern void action_kook_tijd_min(lv_event_t * e);
extern void action_kook_tijd_plus(lv_event_t * e);
extern void action_kook_hop_min(lv_event_t * e);
extern void action_kook_hop_plus(lv_event_t * e);
extern void action_kook_hop_toevoegen(lv_event_t * e);
extern void action_kook_hop_verwijderen(lv_event_t * e);
extern void action_kook_annuleren(lv_event_t * e);
extern void action_kook_start(lv_event_t * e);
extern void action_stap_select(lv_event_t * e);
extern void action_sg_invullen(lv_event_t * e);

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_EVENTS_H*/