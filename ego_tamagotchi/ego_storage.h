#pragma once
#include "ego_core.h"

/* true - состояние прочитано; false - файла нет/битый, state не тронут. */
bool ego_storage_load(EgoState* state);
bool ego_storage_save(const EgoState* state);

/* Лог событий на SD: apps_data/ego_tamagotchi/ego_log.csv, строка = одно событие.
 * event: growth, caught, depart, return, burn, miss, quest, pause.
 * id: номер пункта журнала; для quest - номер задания; для return - секунды вне ЭГО.
 * delta: изменение EGO; ego: значение после события. */
void ego_log(uint32_t ts, const char* event, int32_t id, int32_t delta, uint8_t ego);
