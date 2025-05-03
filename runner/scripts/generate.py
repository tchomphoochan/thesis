#!/usr/bin/env python3

import random
import argparse

def generate_transactions(n_txns, n_objs, max_objs_per_txn, write_prob, seed=None):
    if seed is not None:
        random.seed(seed)

    transactions = []

    for txn_id in range(n_txns):
        aux_data = txn_id  # simple: aux = txn id
        n_accesses = random.randint(1, max_objs_per_txn)
        accesses = []

        used_objs = set()
        cnt_write = 0
        cnt_read = 0
        for _ in range(n_accesses):
            objid = random.randint(0, n_objs-1)
            while objid in used_objs:
                objid = random.randint(0, n_objs-1)
            used_objs.add(objid)

            writeflag = 1 if random.random() < write_prob else 0
            if writeflag:
              cnt_write += 1
            else:
              cnt_read += 1
            accesses.append((objid, writeflag))

        # Format line
        line = [str(aux_data)] + [f"{objid},{writeflag}" for objid, writeflag in accesses]
        transactions.append(",".join(line))

    return transactions

def main():
    parser = argparse.ArgumentParser(description="Generate random transactions.csv file")
    parser.add_argument('--output', required=True, help='Output CSV file path')
    parser.add_argument('--n_txns', type=int, required=True, help='Number of transactions')
    parser.add_argument('--n_objs', type=int, required=True, help='Number of unique objects')
    parser.add_argument('--max_objs_per_txn', type=int, default=4, help='Max number of objects per txn')
    parser.add_argument('--write_probability', type=float, default=0.5, help='Probability of a write (0-1)')
    parser.add_argument('--seed', type=int, default=None, help='Random seed (optional)')
    args = parser.parse_args()

    txns = generate_transactions(
        n_txns=args.n_txns,
        n_objs=args.n_objs,
        max_objs_per_txn=args.max_objs_per_txn,
        write_prob=args.write_probability,
        seed=args.seed
    )

    with open(args.output, 'w') as f:
        for line in txns:
            f.write(line + '\n')

    print(f"✅ Generated {args.n_txns} transactions into '{args.output}'")

if __name__ == '__main__':
    main()

