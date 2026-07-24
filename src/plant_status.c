#include <stdio.h>
#include "plant_status.h"
#include "sequence.h"
#include "gpio_ctrl.h"

const char *plant_level_str(plant_level_t level)
{
    switch (level) {
    case PLANT_OK:          return "OK";
    case PLANT_ATTENZIONE:  return "ATTENZIONE";
    case PLANT_ANOMALIA:    return "ANOMALIA";
    case PLANT_IMPOSSIBILE: return "IMPOSSIBILE";
    default:                return "?";
    }
}

plant_level_t plant_status_evaluate(char *desc, size_t desc_len)
{
    sequence_state_t seq = sequence_get_state();

    int in0 = gpio_ctrl_exp_in_get(EXP_IN_0);
    int in1 = gpio_ctrl_exp_in_get(EXP_IN_1);
    int in2 = gpio_ctrl_exp_in_get(EXP_IN_2);
    int in3 = gpio_ctrl_exp_in_get(EXP_IN_3);

    int out0 = gpio_ctrl_exp_out_get(EXP_OUT_0);
    int out1 = gpio_ctrl_exp_out_get(EXP_OUT_1);
    int out2 = gpio_ctrl_exp_out_get(EXP_OUT_2);

    /* 1. Combinazione sensori fisicamente impossibile (priorita' massima) */
    if (in2 == 1 && in3 == 1) {
        snprintf(desc, desc_len,
                 "IN2 e IN3 sullo stesso serbatoio non possono essere entrambi 1 "
                 "(sensore/cablaggio)");
        return PLANT_IMPOSSIBILE;
    }

    /* 2. OUT1 (impulso avviamento) attivo fuori da RUNNING */
    if (out1 == 1 && seq != SEQ_RUNNING) {
        snprintf(desc, desc_len,
                 "OUT1 attivo fuori da RUNNING - rele' avviamento bloccato");
        return PLANT_ANOMALIA;
    }

    switch (seq) {

    case SEQ_IDLE:
        if (out0 == 1) {
            snprintf(desc, desc_len,
                     "IDLE ma OUT0=1 - rele' generatore bloccato o stato non sincronizzato");
            return PLANT_ANOMALIA;
        }
        if (out2 == 1) {
            snprintf(desc, desc_len,
                     "IDLE ma OUT2=1 - rele' pompa bloccato, nessuna sequenza attiva");
            return PLANT_ANOMALIA;
        }
        if (in0 == 1) {
            snprintf(desc, desc_len,
                     "Generatore acceso a mano, sequenza non coinvolta");
            return PLANT_OK;
        }
        if (in3 == 1) {
            snprintf(desc, desc_len,
                     "Impianto fermo - versamento vuoto (attesa autostart/intervento)");
            return PLANT_ATTENZIONE;
        }
        if (in1 == 1 && in2 == 1) {
            snprintf(desc, desc_len,
                     "Impianto fermo - pescaggio vuoto e versamento pieno (informativo)");
            return PLANT_OK;
        }
        if (in1 == 1) {
            snprintf(desc, desc_len,
                     "Impianto fermo - pescaggio vuoto (informativo)");
            return PLANT_OK;
        }
        if (in2 == 1) {
            snprintf(desc, desc_len,
                     "Impianto fermo - versamento pieno (informativo)");
            return PLANT_OK;
        }
        snprintf(desc, desc_len, "Impianto fermo, tutto a riposo");
        return PLANT_OK;

    case SEQ_RUNNING:
        if (out2 == 1) {
            snprintf(desc, desc_len,
                     "RUNNING ma OUT2=1 - pompa attiva prima del generatore confermato");
            return PLANT_ANOMALIA;
        }
        if (out0 == 0) {
            if (sequence_is_stop_pending()) {
                snprintf(desc, desc_len,
                         "Spegnimento in corso (STOP ricevuto) - uscite gia' spente, "
                         "stato in aggiornamento");
                return PLANT_ATTENZIONE;
            }
            snprintf(desc, desc_len,
                     "RUNNING ma OUT0=0 - generatore non comandato durante l'accensione");
            return PLANT_ANOMALIA;
        }
        snprintf(desc, desc_len, "Accensione generatore in corso");
        return PLANT_OK;

    case SEQ_GEN_OK:
        if (out2 == 1) {
            snprintf(desc, desc_len,
                     "GEN_OK ma OUT2=1 - incoerente (dovrebbe essere POMPA_ON)");
            return PLANT_ANOMALIA;
        }
        if (in0 == 0) {
            snprintf(desc, desc_len,
                     "Generatore fermo inaspettatamente (IN0 perso) - spegnimento in corso");
            return PLANT_ATTENZIONE;
        }
        if (out0 == 1) {
            snprintf(desc, desc_len,
                     "Generatore acceso dalla sequenza, pompa non richiesta");
        } else {
            snprintf(desc, desc_len,
                     "Generatore acceso a mano, sequenza in attesa (aggancio pompa o T4)");
        }
        return PLANT_OK;

    case SEQ_POMPA_ON:
        if (out2 == 0) {
            if (sequence_is_stop_pending()) {
                snprintf(desc, desc_len,
                         "Spegnimento in corso (STOP ricevuto) - uscite gia' spente, "
                         "stato in aggiornamento");
                return PLANT_ATTENZIONE;
            }
            snprintf(desc, desc_len,
                     "POMPA_ON ma OUT2=0 - incoerente, la pompa dovrebbe essere attiva");
            return PLANT_ANOMALIA;
        }
        if (in0 == 0) {
            snprintf(desc, desc_len,
                     "Generatore fermo inaspettatamente (IN0 perso) durante il pompaggio");
            return PLANT_ATTENZIONE;
        }
        if (in1 == 1) {
            snprintf(desc, desc_len,
                     "Pescaggio vuoto - spegnimento pompa in corso");
            return PLANT_ATTENZIONE;
        }
        if (in2 == 1) {
            snprintf(desc, desc_len,
                     "Versamento pieno - spegnimento pompa in corso");
            return PLANT_ATTENZIONE;
        }
        if (out0 == 1) {
            snprintf(desc, desc_len, "Generatore e pompa in funzione, tutto normale");
        } else {
            snprintf(desc, desc_len, "Pompa in funzione (generatore acceso a mano)");
        }
        return PLANT_OK;

    default:
        snprintf(desc, desc_len, "Stato sequenza sconosciuto");
        return PLANT_ANOMALIA;
    }
}
