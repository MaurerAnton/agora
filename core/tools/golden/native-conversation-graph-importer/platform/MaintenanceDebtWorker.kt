// Stand-local com.newoether.agora.service.MaintenanceDebtWorker.
//
// NativeConversationGraphImporter takes `scheduleMaintenance` as a constructor parameter whose
// default value references MaintenanceDebtWorker.schedule(); the scenarios always inject their
// own recording lambda, so this stub exists only for the default argument to compile.
package com.newoether.agora.service

class MaintenanceDebtWorker {
    companion object {
        fun schedule() {
            // no-op in the stand: the driver injects scheduleMaintenance itself
        }
    }
}
