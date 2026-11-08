#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "rust-monitor.h"

int main(int argc, char **argv)
{
    assert(argc == 2);
    MonitorSnapshot *snapshot = monitor_collect(argv[1]);
    assert(snapshot != NULL);
    assert(snapshot->text != NULL && strlen(snapshot->text) > 0);
    assert(snapshot->tooltip != NULL);
    assert(snapshot->interval_seconds >= 1 && snapshot->interval_seconds <= 3600);
    monitor_snapshot_free(snapshot);
    monitor_snapshot_free(NULL);
    return 0;
}
