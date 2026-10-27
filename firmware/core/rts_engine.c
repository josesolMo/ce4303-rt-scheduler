#include "rts_engine.h"
#include <stdio.h>
#include <string.h>
#include "ut.h"

// Funciones internas


static void emit(rts_engine_t *e, uint8_t type, uint8_t task_id, uint8_t prev_id, uint32_t value){

    // Si no hay callback no hace nada
    if (e -> on_event == NULL){
        return; 
    }

    rts_event_t ev;
    ev.type = type;
    ev.task_id = task_id;
    ev.prev_id = prev_id;
    ev.tick = e -> now; 
    ev.value = value;
    e -> on_event(&ev, e -> ctx); 

}

// Funcion que libera un trabajo nuevo de la tarea t en el tick actual
static void release_job(rts_engine_t *e, rts_task_t *t){

    // Si el trabajo anterior sigue pendiente (puede pasar si D > T),
    // se cuenta como plazo perdido y se descarta
    if (t -> active) {
        t -> active = false;
        t -> misses++;
        emit(e, RTS_EV_DEADLINE_MISS, t->info.id, 0, t->remaining_ut);
    }

    t -> active = true;
    t -> remaining_ut = t -> info.W;
    t -> release_tick = e -> now;
    t -> abs_deadline = e -> now + t -> info.D;
    t -> releases++;
    emit(e, RTS_EV_RELEASE, t->info.id, 0, t->abs_deadline);

}

// Retorna true si la tarea a tiene mas prioridad que b en este momento
static bool higher_priority(const rts_engine_t *e, const rts_task_t *a, const rts_task_t *b){

    if (e -> algo == RTS_RMS){

        // RMS: menor periodo es mayor prioridad (fija)
        if (a -> info.T != b -> info.T){
            return a -> info.T < b -> info.T; // True si T de a es < que T de b
        }

    } else {

        // EDF: plazo absoluto mas cercano es mayor prioridad
        if (a -> abs_deadline != b -> abs_deadline){
            return a -> abs_deadline < b -> abs_deadline;
        }

    }

    // Desempate: mayor criticidad y luego menor id
    if (a -> info.priority != b -> info.priority){
        return a -> info.priority > b -> info.priority;
    }

    return a -> info.id < b -> info.id;

}


// Funciones publicas


// Funcion para inicializar el calendarizador (lo deja vacio y detenido)
void rts_init (rts_engine_t *e, rts_algo_t algo, uint32_t ut_per_tick, rts_event_cb_t on_event, void *ctx){

    memset(e, 0, sizeof(*e));
    e -> algo = algo;
    e -> ut_per_tick = (ut_per_tick == 0) ? 1 : ut_per_tick;
    e -> current = -1;
    e -> last_id = RTS_IDLE;
    e -> on_event = on_event;
    e -> ctx = ctx;

}


// Funcion para anadir una tarea si cumple con los requisitos
// Devuelve: 0 si todo bien, -1 si hubo un error
int rts_add_task(rts_engine_t *e, const Task *t){

    if (t == NULL || e -> count >= RTS_MAX_TASKS){
        return -1;
    }

    if (t -> W == 0 || t -> T == 0 || t -> D == 0 || t -> id == RTS_IDLE){
        return -1;
    }

    if (t -> type != 'P' && t -> type != 'S'){
        return -1;
    }

    for (int i = 0; i < e -> count; i++){

        // id repetido
        if (e -> tasks[i].info.id == t -> id){
            return -1;
        }

        // maximo una esporadica por nodo
        if (t -> type == 'S' && e -> tasks[i].info.type == 'S'){
            return -1;
        }

    }

    rts_task_t *nt = &e -> tasks[e -> count]; // Asignamos la tarea en la posicion que le corresponde del array
    memset(nt, 0, sizeof(*nt));
    nt -> info = *t;

    // Si ya esta corriendo, la primera liberacion es en el tick actual
    nt -> next_release = e -> running ? e -> now : 0;

    e -> count++; // Aumentamos el contador de tareas
    return 0;

}

// Funcion para arrancar el calendarizador (tick cero), conserva las tareas
void rts_start(rts_engine_t *e){

    e -> now = 0; // tick cero
    e -> current = -1; // No hay una tarea current
    e -> last_id = RTS_IDLE; // Todavia no hay tareas
    e -> busy_ticks = 0; // Todavia no ha estado ocupado

    // Iniciamos desde el tick cero todas las tareas del array, asi todas empiezan igual
    for (int i = 0; i < e -> count; i++){

        rts_task_t *t = &e -> tasks[i];
        t -> active = false;
        t -> next_release = 0;
        t -> sporadic_pending = false;
        t -> has_released = false;
        t -> releases = t -> completions = t -> misses = t -> ignored = 0;
        
    }

    e -> running = true;
}

// Funcion para detener el calendarizador
void rts_stop(rts_engine_t *e){

    e -> running = false;
    e -> current = -1;

}

// Funcion para el caso de que se presione el boton que ejecuta la tarea esporadica
void rts_sporadic_trigger(rts_engine_t *e){

    // Recorremos el array de tareas hasta encontrar la esporadica
    for (int i = 0; i < e -> count; i++){
        if (e -> tasks[i].info.type == 'S'){
            e -> tasks[i].sporadic_pending = true; // Queda pendiente, se libera en el siguiente rts_tick()
            return;
        }       
    }
}

// Funcion que devuelve el id de la tarea por tick
uint8_t rts_tick(rts_engine_t *e){

    e -> current = -1;

    // Si no esta corriendo el calendarizador
    if (!e -> running){
        return RTS_IDLE;
    }

    // En caso de que venza el plazo (el trabajo debia terminar antes de este tick)
    for (int i = 0; i < e -> count; i++){

        rts_task_t *t = &e -> tasks[i];

        // Si esta activo y el tick actual es mayor o igual al deadline
        if(t -> active && e -> now >= t -> abs_deadline){

            // Abortamos el trabajo, aumentamos el contador de misses y lo reportamos
            t -> active = false;
            t -> misses++;
            emit(e, RTS_EV_DEADLINE_MISS, t -> info.id, 0, t -> remaining_ut);
        }

    }

    // Para la liberaciones de tareas
    for (int i = 0; i < e -> count; i++){

        rts_task_t *t = &e -> tasks[i];

        if (t -> info.type == 'P'){

            // Si ya se puede liberar
            if (e -> now >= t -> next_release){

                release_job(e, t); // Liberamos la tarea
                t -> next_release += t -> info.T; // Actualizamos la proxima liberacion
            }
        // Si es esporadica
        } else if(t -> sporadic_pending){

            t -> sporadic_pending = false;

            // Si aun no ha pasado el intervalo minimo T, se ignora
            if (t -> has_released && (e -> now - t -> last_release) < t -> info.T){

                t -> ignored++;
                emit(e, RTS_EV_SPORADIC_IGNORED, t -> info.id, 0, e -> now - t -> last_release);

            } else {

                release_job(e, t);
                t -> has_released = true;
                t -> last_release = e -> now;

            }

        }

    }

    // Ahora, se elige la tarea que tiene mayor prioridad entre las que tienen trabajo
    for (int i = 0; i < e -> count; i++){

        rts_task_t *t = &e -> tasks[i];

        // Si no esta activa continuamos
        if (!t -> active){
            continue;
        }

        // Si no hay una tarea o si la tarea a tiene mayor prioridad que la actual (b)
        if (e -> current < 0 || higher_priority(e, t, &e -> tasks[e -> current])){

            e -> current = i; // Asignamos como la tarea actual t

        }
    }

    // Reportamos el cambio de tarea

    // id = RTS_IDLE si no hay ninguna tarea con trabajo (current < 0), si no, el id de la tarea elegida
    uint8_t id = (e -> current >= 0) ? e -> tasks[e -> current].info.id : RTS_IDLE;

    if (id != e -> last_id){

        emit(e, RTS_EV_SWITCH, id, e -> last_id, 0);
        e -> last_id = id; // Actualizamos al id de la actual

    }

    return id;

}

void rts_run (rts_engine_t *e){

    if (!e -> running){
        return;
    }

    if (e -> current >= 0){

        rts_task_t *t = &e -> tasks[e -> current];

        // Trabajo de un tick: hasta s_n unidades de trabajo

        uint32_t n = t -> remaining_ut;

        // Si ut que falta es mayor que los ut por tick
        if(n > e -> ut_per_tick){
            n = e -> ut_per_tick;
        }

        // Ejecutamos la cantidad de ut que corresponden por tick
        for (uint32_t i = 0; i < n; i++){
            unidad_trabajo();
        }

        t -> remaining_ut -= n; // Se resta para obtener el trabajo restante
        e -> busy_ticks++;

        // Si ya termino el trabajo
        if (t -> remaining_ut == 0){

            t -> active = false;
            t -> completions++;
            // +1 porque termina al final de este tick
            emit(e, RTS_EV_COMPLETE, t -> info.id, 0, e -> now + 1 - t -> release_tick);

        }
    }

    e -> now++;

}