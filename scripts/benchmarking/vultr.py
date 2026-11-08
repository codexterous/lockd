"""Journal creation intents and delete only instances whose ownership still matches."""

import contextlib
import datetime
import json
import os
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from collections.abc import Iterator
from pathlib import Path

from .config import object_value

IDENTITY = ('id', 'date_created', 'label', 'region', 'plan', 'os_id')
REJECTED_CREATE_STATUSES = frozenset((400, 401, 403, 404, 422, 429))
MAX_ERROR_BODY_BYTES = 8192
MAX_ERROR_DETAIL_CHARS = 500
INSTANCE_VISIBILITY_TIMEOUT_SECONDS = 120
INSTANCE_POLL_INTERVAL_SECONDS = 5
DELETION_TIMEOUT_SECONDS = 60


def provider_error(body: bytes, token: str, data: dict | None) -> str | None:
    """
    Extract a bounded provider reason without echoing credentials or private inputs.
    """
    try:
        response = json.loads(body)
    except ValueError:
        return None
    detail = response.get('error') if isinstance(response, dict) else None
    if not isinstance(detail, str):
        return None
    redactions = [token]

    # Only public deployment choices are safe to echo. Other request strings
    # include SSH/VPC IDs, cleanup tags and potentially future credential fields.
    public_fields = {'region', 'plan', 'os_id', 'backups', 'enable_ipv6', 'user_scheme'}
    pending = [value for key, value in (data or {}).items() if key not in public_fields]
    while pending:
        value = pending.pop()
        if isinstance(value, str):
            redactions.append(value)
        elif isinstance(value, list):
            pending.extend(value)
        elif isinstance(value, dict):
            pending.extend(value.values())
    for value in sorted(set(redactions), key=len, reverse=True):
        if value:
            detail = detail.replace(value, '[redacted]')
    detail = ' '.join(
        ''.join(char if char.isprintable() else ' ' for char in detail).split()
    )
    return detail[:MAX_ERROR_DETAIL_CHARS] or None


class ApiError(RuntimeError):
    """Report API failures with a bounded, redacted provider reason when available."""

    def __init__(
        self, status: int, method: str, path: str, detail: str | None = None
    ) -> None:
        message = f"Vultr {method} {path} returned HTTP {status}"
        super().__init__(message + ': ' + detail if detail else message)
        self.status = status
        self.detail = detail


class NoRedirect(urllib.request.HTTPRedirectHandler):
    """Keep bearer credentials on the fixed API origin, including on redirects."""

    def redirect_request(
        self,
        req: urllib.request.Request,
        fp: object,
        code: int,
        msg: str,
        headers: object,
        newurl: str,
    ) -> None:
        """Reject rather than forward an authenticated request elsewhere."""
        raise ApiError(code, req.get_method(), req.selector)


class Api:
    """Call Vultr v2 without retrying ambiguous creation or deletion requests."""

    def __init__(self, token: str) -> None:
        if not token:
            raise ValueError("Set VULTR_API_KEY on the controller")
        self._token = token
        self._opener = urllib.request.build_opener(NoRedirect())
        self._request_lock = threading.Lock()

    def request(self, method: str, path: str, data: dict | None = None) -> dict:
        """Serialize the shared opener while worker SSH setup proceeds in parallel."""
        with self._request_lock:
            return self._request(method, path, data)

    def _request(self, method: str, path: str, data: dict | None = None) -> dict:
        """Bound network waits and redact potentially sensitive API errors."""
        if not path.startswith('/') or path.startswith('//'):
            raise ValueError("Expected a relative API path")
        body = json.dumps(data).encode() if data is not None else None
        request = urllib.request.Request(
            'https://api.vultr.com/v2' + path,
            data=body,
            method=method,
            headers={
                'Authorization': 'Bearer ' + self._token,
                'Content-Type': 'application/json',
                'User-Agent': 'lockd-benchmark',
            },
        )
        try:
            with self._opener.open(request, timeout=30) as response:
                raw = response.read()
        except urllib.error.HTTPError as error:
            try:
                detail = provider_error(
                    error.read(MAX_ERROR_BODY_BYTES), self._token, data
                )
            except OSError:
                detail = None
            finally:
                error.close()
            raise ApiError(error.code, method, path, detail) from None
        except urllib.error.URLError:
            raise RuntimeError(
                f"Vultr {method} {path} failed; response may be lost"
            ) from None
        return object_value(json.loads(raw)) if raw else {}

    def items(self, path: str, key: str) -> list[dict]:
        """Follow every cursor without following API-supplied absolute URLs."""
        values: list[dict] = []
        cursor = ''
        seen: set[str] = set()
        while True:
            parameters: dict[str, int | str] = {'per_page': 100}

            # An empty cursor is rejected by /os. The initial page has no cursor;
            # subsequent pages use exactly the cursor returned by the API.
            if cursor:
                parameters['cursor'] = cursor
            query = urllib.parse.urlencode(parameters)
            response = self.request('GET', path + '?' + query)
            values.extend(object_value(item) for item in response[key])
            cursor = response.get('meta', {}).get('links', {}).get('next', '')
            if not cursor:
                return values
            if cursor in seen:
                raise ValueError("Vultr returned a repeated pagination cursor")
            seen.add(cursor)

    def instance(self, instance_id: str) -> dict:
        """Fetch only the requested UUID; never use labels as deletion targets."""
        instance_id = str(uuid.UUID(instance_id))
        return object_value(
            self.request('GET', '/instances/' + instance_id)['instance']
        )


def save(path: Path, state: dict) -> None:
    """
    Replace the journal atomically after flushing each creation/deletion transition.
    """
    temporary = path.with_suffix('.tmp')
    with temporary.open('w', encoding='utf-8', newline='\n') as stream:
        json.dump(state, stream, indent=2)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)
    if os.name != 'nt':
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)


@contextlib.contextmanager
def journal_lock(path: Path) -> Iterator[None]:
    """Prevent concurrent run/cleanup commands; OS locks release after a crash."""
    with path.with_suffix('.lock').open('a+b') as stream:
        stream.write(b'0')
        stream.flush()
        stream.seek(0)
        if os.name == 'nt':
            import msvcrt

            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl

            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            yield
        finally:
            stream.seek(0)
            if os.name == 'nt':
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(stream, fcntl.LOCK_UN)


def load(path: Path) -> dict:
    """Refuse incomplete or unfamiliar journals before considering any deletion."""
    state = object_value(json.loads(path.read_text(encoding='utf-8')))
    if state.get('schema') != 1 or str(uuid.UUID(state['run_id'])) != state['run_id']:
        raise ValueError("Invalid benchmark journal")
    if not isinstance(state['resources'], list):
        raise TypeError("Invalid resource journal")
    labels: set[str] = set()
    identifiers: set[str] = set()
    for record in state['resources']:
        role = record['role']
        if role != 'daemon' and (
            not role.startswith('worker-')
            or not role.removeprefix('worker-').isdigit()
            or not 0 <= int(role.removeprefix('worker-')) < 100
        ):
            raise ValueError("Invalid benchmark role")
        expected_label = f'lockd-bench-{state["run_id"][:8]}-{role}'
        if record['label'] in labels or record['label'] != expected_label:
            raise ValueError("Invalid or duplicate resource label")
        labels.add(record['label'])
        uuid.UUID(record['token'])
        if record.get('id'):
            uuid.UUID(record['id'])
            if record['id'] in identifiers:
                raise ValueError("Duplicate resource UUID")
            identifiers.add(record['id'])
        if record['status'] not in (
            'pending',
            'created',
            'deleting',
            'deleted',
            'rejected',
        ):
            raise ValueError("Invalid resource status")
    return state


def owned(record: dict, instance: dict, run_id: str) -> bool:
    """Require the exact creation intent, random tags, and returned identity."""
    expected = record['request']
    if any(
        instance.get(key) != expected[key]
        for key in ('label', 'region', 'plan', 'os_id')
    ):
        return False
    tags = instance.get('tags', [])
    if not isinstance(tags, list) or not {
        'lockd-bench-' + run_id,
        record['token'],
    }.issubset(tags):
        return False
    if record.get('id') and record['id'] != instance.get('id'):
        return False
    created = instance.get('date_created')
    if not isinstance(created, str) or not created:
        return False
    try:
        date = datetime.datetime.fromisoformat(created)
        date = date.replace(tzinfo=datetime.UTC) if date.tzinfo is None else date
        if not record['intent_unix'] - 120 <= date.timestamp() <= time.time() + 120:
            return False
    except ValueError:
        return False
    snapshot = record.get('identity')
    return snapshot is None or all(
        instance.get(key) == snapshot[key] for key in IDENTITY
    )


def supports_local_boot(plan: dict) -> bool:
    """Use Vultr's storage classification, not a positive disk size or plan name."""

    # Block-only VX1 plans report disk=1 despite having no local boot disk.
    # Missing or unfamiliar classifications must not implicitly buy a volume.
    return (
        plan.get('storage_type') in ('local_storage', 'local_and_block_storage')
        and plan['disk'] > 0
    )


def create(
    api: Api, path: Path, state: dict, config: dict, role: str, plan: dict
) -> dict:
    """Write intent before POST, preserving ambiguous failures for explicit recovery."""
    if not supports_local_boot(plan):
        raise ValueError(f"The {role} plan {plan['id']} needs local storage")
    token = str(uuid.uuid4())
    label = f'lockd-bench-{state["run_id"][:8]}-{role}'
    request = {
        'region': config['region'],
        'plan': plan['id'],
        'os_id': config['os_id'],
        'label': label,
        'tags': ['lockd-bench-' + state['run_id'], token],
        'sshkey_id': [config['ssh_key_id']],
        'attach_vpc': [config['vpc_id']],
        'firewall_group_id': config['firewall_group_id'],
        'backups': 'disabled',
        'enable_ipv6': False,
        'user_scheme': 'root',
    }
    if plan['type'] == 'vx1':
        # VX1 needs an explicit local boot selection. No disk_size or volume UUID
        # is supplied: those would create or attach separately managed storage.
        request['block_devices'] = [{'block_id': 'local', 'bootable': True}]
    record = {
        'role': role,
        'label': label,
        'token': token,
        'request': request,
        'intent_unix': time.time(),
        'status': 'pending',
    }
    state['resources'].append(record)
    save(path, state)
    try:
        response = api.request('POST', '/instances', request)
    except ApiError as error:
        # Explicit request rejections cannot have accepted this creation. A lost
        # response, timeout or server error remains pending for safe reconciliation.
        if error.status in REJECTED_CREATE_STATUSES:
            record.update(
                status='rejected',
                creation_error={
                    'status': error.status,
                    'detail': error.detail,
                },
            )
            save(path, state)
        raise
    instance = object_value(response['instance'])
    # The returned UUID is journaled even if other response fields are incomplete.
    record['id'] = str(uuid.UUID(instance['id']))
    record['status'] = 'created'
    save(path, state)
    return record


def missing(error: ApiError) -> bool:
    """Classify a not-found response without assuming why the instance is missing."""
    return error.status == 404


def wait_for_instance(
    api: Api,
    instance_id: str,
    timeout: float,
    *,
    cancelled: threading.Event | None = None,
) -> dict:
    """Wait for a UUID to become readable without resubmitting its creation."""
    deadline = time.monotonic() + timeout
    while True:
        if cancelled is not None and cancelled.is_set():
            raise InterruptedError('Worker preparation cancelled before API lookup')
        try:
            return api.instance(instance_id)
        except ApiError as error:
            if not missing(error):
                raise
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(
                    f"Instance {instance_id} is still not visible after {timeout:g}s; "
                    "creation and deletion remain unconfirmed. Keep the recovery journal."
                ) from error

            # A successful create can be followed by a temporarily missing GET.
            # Retry this UUID only; repeating POST could buy another machine.
            print(
                f'Waiting for instance {instance_id} to become visible: '
                f'Vultr returned 404; {remaining:.0f}s remaining',
                flush=True,
            )
            interval = min(INSTANCE_POLL_INTERVAL_SECONDS, remaining)
            if cancelled is None:
                time.sleep(interval)
            else:
                cancelled.wait(interval)


def confirm_deletion(api: Api, path: Path, state: dict, record: dict) -> bool:
    """Confirm absence only after the provider has acknowledged this run's DELETE."""
    if not record.get('delete_acknowledged'):
        raise ValueError('Cannot confirm deletion without an acknowledged DELETE')
    deadline = time.monotonic() + DELETION_TIMEOUT_SECONDS
    while True:
        try:
            api.instance(record['id'])
        except ApiError as error:
            if not missing(error):
                raise
            record['status'] = 'deleted'
            save(path, state)
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(2)


def cleanup(api: Api, path: Path, state: dict) -> list[str]:
    """Delete journal-owned machines after rechecking identity, then verify absence."""
    remaining: list[str] = []
    for record in state['resources']:
        if record['status'] in ('deleted', 'rejected'):
            continue
        try:
            if record.get('delete_acknowledged'):
                if not confirm_deletion(api, path, state, record):
                    remaining.append(record['id'] + ': deletion not yet confirmed')
                continue
            if record.get('id'):
                candidates = [
                    wait_for_instance(
                        api, record['id'], INSTANCE_VISIBILITY_TIMEOUT_SECONDS
                    )
                ]
            else:
                # A lost POST response is never retried. Only its exact saved intent
                # can nominate a candidate. Copied labels alone are insufficient.
                candidates = [
                    item
                    for item in api.items('/instances', 'instances')
                    if owned(record, item, state['run_id'])
                ]
                if len(candidates) != 1:
                    remaining.append(
                        record['label'] + ': unresolved creation; '
                        f'{len(candidates)} matching instances currently visible'
                    )
                    continue
            instance = candidates[0]
            if not owned(record, instance, state['run_id']):
                remaining.append(record['label'] + ': identity mismatch; refused')
                continue
            instance_id = str(uuid.UUID(instance['id']))
            # Re-fetch before deletion. A recorded UUID alone does not establish
            # ownership when a journal is wrong or the instance has changed.
            current = wait_for_instance(
                api, instance_id, INSTANCE_VISIBILITY_TIMEOUT_SECONDS
            )
            if not owned(record, current, state['run_id']) or any(
                current.get(key) != instance.get(key)
                for key in (*IDENTITY, 'main_ip', 'internal_ip', 'tags')
            ):
                remaining.append(instance_id + ': identity changed; refused')
                continue
            record.update(
                id=instance_id,
                identity={key: current[key] for key in IDENTITY},
                status='deleting',
            )
            save(path, state)
            print('Deleting ' + record['label'] + ' (' + instance_id + ')', flush=True)
            api.request('DELETE', '/instances/' + instance_id)
            record['delete_acknowledged'] = True
            save(path, state)
            if not confirm_deletion(api, path, state, record):
                remaining.append(instance_id + ': deletion not yet confirmed')
        except ApiError as error:
            remaining.append(record['label'] + ': ' + str(error))
        except (OSError, ValueError, RuntimeError) as error:
            remaining.append(record['label'] + ': ' + str(error))
    for item in remaining:
        print('NOT REMOVED: ' + item)
    return remaining
