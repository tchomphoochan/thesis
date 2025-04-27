#!/usr/bin/env python3

import sys
import re
import os
import io
from collections import defaultdict, namedtuple

# --- Structures ---

Transaction = namedtuple('Transaction', ['reads', 'writes'])

class LogEntry:
    def __init__(self, kind, time, txn_id, puppet_id=None):
        self.kind = kind
        self.time = time
        self.txn_id = txn_id
        self.puppet_id = puppet_id

# --- Parsing Functions ---

def parse_transactions_csv(fileobj):
    txn_map = {}

    for lineno, line in enumerate(fileobj, 1):
        line = line.strip()
        if not line:
            continue
        parts = line.split(',')
        try:
            aux_data = int(parts[0])
            objs = parts[1:]
        except ValueError:
            sys.exit(f"Error parsing aux_data at line {lineno}")

        reads, writes = [], []
        if len(objs) % 2 != 0:
            sys.exit(f"Error: line {lineno} has incomplete objid/writeflag pairs")

        for i in range(0, len(objs), 2):
            objid = int(objs[i])
            writeflag = int(objs[i+1])
            if writeflag:
                writes.append(objid)
            else:
                reads.append(objid)

        txn_map[lineno-1] = Transaction(reads, writes)
    return txn_map

def parse_log(fileobj):
    init_re = re.compile(r'(.*[xX]sim.*)|(.*veril.*)')
    submit_re = re.compile(r'\[\+\s*([0-9.]+)\] submit txn id=(\d+)(?: aux=\d+)?')
    scheduled_re = re.compile(r'\[\+\s*([0-9.]+)\] scheduled txn id=(\d+) assigned to puppet (\d+)')
    done_re = re.compile(r'\[\+\s*([0-9.]+)\] done puppet (\d+) finished txn id=(\d+)')

    events = []

    for line in fileobj:
        line = line.strip()
        if not line:
            continue
        if m := init_re.search(line):
            continue
        elif m := submit_re.search(line):
            events.append(LogEntry('submit', float(m.group(1)), int(m.group(2))))
        elif m := scheduled_re.search(line):
            events.append(LogEntry('scheduled', float(m.group(1)), int(m.group(2)), puppet_id=int(m.group(3))))
        elif m := done_re.search(line):
            events.append(LogEntry('done', float(m.group(1)), int(m.group(3)), puppet_id=int(m.group(2))))
        else:
            sys.exit(f"Failed to parse: {line}")
    return events


# --- Conflict Checking ---

def conflict(txnA, txnB):
    for w in txnA.writes:
        if w in txnB.reads or w in txnB.writes:
            return True
    for r in txnA.reads:
        if r in txnB.writes:
            return True
    return False

# --- Consistency + Metric Checks ---

def check_consistency(txn_map, events, num_puppets):
    submitted = set()
    scheduled = set()
    done = set()

    submit_times = []
    schedule_times = {}
    done_times = {}

    active_txns = {}

    puppet_busy_time = defaultdict(float)

    first_submit_time = None
    last_done_time = None

    for e in events:
        if e.kind == 'submit':
            if e.txn_id in submitted:
                sys.exit(f"Error: Transaction {e.txn_id} submitted more than once")
            submitted.add(e.txn_id)
            submit_times.append(e.time)
            if first_submit_time is None or e.time < first_submit_time:
                first_submit_time = e.time

        elif e.kind == 'scheduled':
            if e.txn_id not in submitted:
                sys.exit(f"Error: Transaction {e.txn_id} scheduled without being submitted first")
            if e.txn_id in scheduled:
                sys.exit(f"Error: Transaction {e.txn_id} scheduled more than once")
            if e.puppet_id < 0 or e.puppet_id >= num_puppets:
                sys.exit(f"Error: Puppet ID {e.puppet_id} out of valid range at txn {e.txn_id}")

            new_txn = txn_map[e.txn_id]
            for other_txn, _ in active_txns.values():
                if conflict(new_txn, other_txn):
                    sys.exit(f"Error: Conflict detected when scheduling txn {e.txn_id}")

            active_txns[e.txn_id] = (new_txn, e.puppet_id)
            scheduled.add(e.txn_id)
            schedule_times[e.txn_id] = (e.time, e.puppet_id)

        elif e.kind == 'done':
            if e.txn_id not in scheduled:
                sys.exit(f"Error: Transaction {e.txn_id} completed without being scheduled first")
            if e.txn_id in done:
                sys.exit(f"Error: Transaction {e.txn_id} completed more than once")
            if e.txn_id not in active_txns:
                sys.exit(f"Error: Transaction {e.txn_id} done but not in active set")

            txn, puppet_id_sched = active_txns[e.txn_id]
            if puppet_id_sched != e.puppet_id:
                sys.exit(f"Error: Puppet ID mismatch on done for txn {e.txn_id}")

            start_time, _ = schedule_times[e.txn_id]
            puppet_busy_time[e.puppet_id] += (e.time - start_time)

            done.add(e.txn_id)
            done_times[e.txn_id] = e.time
            last_done_time = e.time

            del active_txns[e.txn_id]

    for txn_id in submitted:
        if txn_id not in scheduled:
            sys.exit(f"Error: Transaction {txn_id} submitted but never scheduled")

    for txn_id in scheduled:
        if txn_id not in done:
            sys.exit(f"Error: Transaction {txn_id} scheduled but never completed")

    print("✅ All consistency checks passed!\n")

    # --- Throughput and Utilization Metrics ---
    total_submitted = len(submit_times)
    if total_submitted > 1:
        submit_duration = max(submit_times) - min(submit_times)
        client_throughput = total_submitted / submit_duration
        print(f"Client submission rate: {client_throughput/1e6:.2f} million transactions/sec")
    else:
        print("Client submission rate: N/A")

    if last_done_time is not None and first_submit_time is not None:
        total_wall_time = last_done_time - first_submit_time
        print(f"Total wall time: {total_wall_time:.6f} seconds")
        print("Puppet utilization:")
        for puppet_id in range(num_puppets):
            busy = puppet_busy_time.get(puppet_id, 0.0)
            utilization = 100.0 * busy / total_wall_time if total_wall_time > 0 else 0.0
            print(f"  Puppet {puppet_id}: {utilization:.2f}%")
    print()

# --- Main Entrypoint ---

def main():
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <transactions.csv> <log.txt> <num_puppets>")
        sys.exit(1)

    with open(sys.argv[1]) as csvf:
        txn_map = parse_transactions_csv(csvf)
    with open(sys.argv[2]) as logf:
        events = parse_log(logf)

    num_puppets = int(sys.argv[3])

    check_consistency(txn_map, events, num_puppets)

# --- Test Cases ---

def _run_tests():
    print("Running built-in tests...")

    csv_good = """0,1,0,2,1
1,3,0,4,1
"""
    log_good = """[+0.001] submit txn id=0 aux=0
[+0.002] submit txn id=1 aux=1
[+0.003] scheduled txn id=0 assigned to puppet 0
[+0.004] scheduled txn id=1 assigned to puppet 1
[+0.005] done puppet 0 finished txn id=0
[+0.006] done puppet 1 finished txn id=1
"""
    txn_map = parse_transactions_csv(io.StringIO(csv_good))
    events = parse_log(io.StringIO(log_good))
    check_consistency(txn_map, events, num_puppets=2)

    print("✅ Built-in tests passed.\n")

if __name__ == '__main__':
    if os.getenv('RUN_TESTS', '0') == '1':
        _run_tests()
    else:
        main()

