R"sql(
CREATE SCHEMA IF NOT EXISTS exsim;
CREATE TABLE IF NOT EXISTS exsim.schema_version (
    singleton boolean PRIMARY KEY DEFAULT true CHECK (singleton),
    version integer NOT NULL,
    artifact_root text NOT NULL
);
CREATE TABLE IF NOT EXISTS exsim.experiments (
    record text NOT NULL CHECK (((record::jsonb->>'schema') = 'exsim.api.experiment-record/1') IS TRUE),
    experiment_id text GENERATED ALWAYS AS (record::jsonb->>'experimentId') STORED PRIMARY KEY,
    CHECK (experiment_id ~ '^exp_[0-9a-f]{16}$')
);
CREATE TABLE IF NOT EXISTS exsim.runs (
    record text NOT NULL CHECK (((record::jsonb->>'schema') = 'exsim.api.run-record/1') IS TRUE),
    run_id text GENERATED ALWAYS AS (record::jsonb->>'runId') STORED PRIMARY KEY,
    experiment_id text GENERATED ALWAYS AS (record::jsonb->>'experimentId') STORED NOT NULL
        REFERENCES exsim.experiments(experiment_id),
    state text GENERATED ALWAYS AS (record::jsonb->>'state') STORED NOT NULL
        CHECK (state IN ('queued','running','paused','completed','cancelled','failed')),
    CHECK (run_id ~ '^run_[0-9a-f]{16}$')
);
CREATE INDEX IF NOT EXISTS runs_experiment_state ON exsim.runs(experiment_id, state);
CREATE INDEX IF NOT EXISTS runs_queued ON exsim.runs(run_id) WHERE state = 'queued';
CREATE TABLE IF NOT EXISTS exsim.idempotency (
    record text NOT NULL CHECK (((record::jsonb->>'schema') = 'exsim.api.idempotency-record/1') IS TRUE),
    key text GENERATED ALWAYS AS (record::jsonb->>'key') STORED PRIMARY KEY,
    fingerprint text GENERATED ALWAYS AS (record::jsonb->>'fingerprint') STORED NOT NULL,
    status integer GENERATED ALWAYS AS ((record::jsonb->>'status')::integer) STORED NOT NULL
        CHECK (status IN (0,200,201)),
    CHECK (key ~ '^[A-Za-z0-9_-]{8,128}$'),
    CHECK (fingerprint ~ '^[0-9a-f]{64}$')
);
)sql"
