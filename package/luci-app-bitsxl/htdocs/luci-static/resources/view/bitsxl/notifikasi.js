'use strict';
'require view';
'require fs';
'require ui';

const BIN = '/usr/bin/bitsxl';
const SOFT_LINE = 'rgba(127,127,127,.16)';
const SOFT_BORDER = 'linear-gradient(transparent,transparent) padding-box,linear-gradient(135deg,rgba(127,127,127,.24),rgba(127,127,127,.08),rgba(127,127,127,.18)) border-box';

function callBitsxl(args) {
	return L.resolveDefault(fs.exec_direct(BIN, args, 'json'), { ok: false, error: _('Unable to execute bitsxl') });
}

function notificationList(payload) {
	const roots = [ payload, payload && payload.notifications, payload && payload.response ];

	for (let index = 0; index < roots.length; index++) {
		const root = roots[index];
		if (!root || typeof root !== 'object')
			continue;

		const data = root.data && typeof root.data === 'object' ? root.data : root;
		const notification = data.notification;
		if (Array.isArray(notification))
			return notification;
		if (notification && Array.isArray(notification.data))
			return notification.data;
	}

	return [];
}

function firstValue(item, keys) {
	if (!item || typeof item !== 'object')
		return '';
	for (let index = 0; index < keys.length; index++) {
		const value = item[keys[index]];
		if (value != null && value !== '')
			return value;
	}
	return '';
}

function textValue(value) {
	if (value == null)
		return '';
	if (typeof value === 'object') {
		try {
			return JSON.stringify(value);
		} catch (err) {
			return String(value);
		}
	}
	return String(value);
}

function notificationId(item) {
	return textValue(firstValue(item, [ 'notification_id', 'id' ])).trim();
}

function notificationRead(item) {
	const value = firstValue(item, [ 'is_read', 'read' ]);
	if (value === true || value === 1)
		return true;
	return [ 'true', '1', 'read', 'yes' ].indexOf(String(value).toLowerCase()) >= 0;
}

function formatTimestamp(value) {
	if (value == null || value === '')
		return '';
	if (typeof value === 'string' && !/^\d+$/.test(value))
		return value;

	const number = Number(value);
	if (!number || isNaN(number))
		return textValue(value);
	const milliseconds = number > 100000000000 ? number : number * 1000;
	try {
		return new Date(milliseconds).toLocaleString('id-ID');
	} catch (err) {
		return textValue(value);
	}
}

function unreadIds(items) {
	const found = {};
	const ids = [];
	items.forEach((item) => {
		const id = notificationId(item);
		if (!notificationRead(item) && id && !found[id]) {
			found[id] = true;
			ids.push(id);
		}
	});
	return ids;
}

function readChunks(ids, offset, progress) {
	if (offset >= ids.length)
		return Promise.resolve();

	const chunk = ids.slice(offset, offset + 64);
	progress.textContent = _('Processing notification') + ' ' + (offset + 1) + '-' + (offset + chunk.length) + ' / ' + ids.length + '…';
	const command = chunk.length === 1 ? 'notification-detail' : 'notification-read-all';

	return callBitsxl([ 'json', command ].concat(chunk)).then((result) => {
		if (!result || !result.ok)
			return Promise.reject((result && (result.error || result.message)) || _('Unable to mark notification as read.'));
		return readChunks(ids, offset + chunk.length, progress);
	});
}

function markRead(ids) {
	if (!ids.length)
		return;

	const progress = E('p', {}, _('Preparing notifications…'));
	ui.showModal(_('Read Notifications'), [ progress ]);

	readChunks(ids, 0, progress).then(() => {
		ui.hideModal();
		window.location.reload();
	}, (error) => {
		ui.hideModal();
		ui.addNotification(null, E('p', {}, textValue(error)), 'warning');
	});
}

function statusChip(read) {
	return E('span', {
		'style': 'display:inline-flex;align-items:center;border:1px solid ' + (read ? SOFT_LINE : '#d97706') + ';border-radius:999px;padding:.2em .6em;background:' + (read ? 'rgba(127,127,127,.08)' : 'rgba(217,119,6,.10)') + ';color:' + (read ? 'inherit' : '#b45309') + ';font-size:.86em;font-weight:700'
	}, read ? _('READ') : _('UNREAD'));
}

function notificationCard(item, index) {
	const read = notificationRead(item);
	const id = notificationId(item);
	const brief = textValue(firstValue(item, [ 'brief_message', 'title', 'subject' ]));
	const full = textValue(firstValue(item, [ 'full_message', 'message', 'description' ]));
	const timestamp = formatTimestamp(firstValue(item, [ 'timestamp', 'created_at', 'date' ]));

	return E('div', {
		'class': 'cbi-section',
		'style': 'margin-top:1em;border:1px solid transparent;border-radius:8px;background:' + SOFT_BORDER + ';padding:1em;opacity:' + (read ? '.78' : '1')
	}, [
		E('div', { 'style': 'display:grid;grid-template-columns:2.5em minmax(0,1fr) auto;gap:.8em;align-items:start' }, [
			E('div', { 'style': 'width:2.5em;height:2.5em;line-height:2.5em;text-align:center;border-radius:50%;background:rgba(127,127,127,.13);font-weight:700' }, String(index + 1)),
			E('div', { 'style': 'min-width:0' }, [
				E('div', { 'style': 'font-size:1.06em;font-weight:700;line-height:1.35;overflow-wrap:anywhere' }, brief || _('Notification')),
				timestamp ? E('div', { 'style': 'margin-top:.25em;color:inherit;opacity:.62' }, timestamp) : ''
			]),
			statusChip(read)
		]),
		full && full !== brief ? E('div', { 'style': 'margin-top:.8em;padding-top:.75em;border-top:1px solid ' + SOFT_LINE + ';white-space:pre-wrap;overflow-wrap:anywhere;line-height:1.5' }, full) : '',
		!read && id ? E('div', { 'style': 'display:flex;justify-content:flex-end;margin-top:.8em' }, [
			E('button', { 'class': 'btn cbi-button cbi-button-action', 'click': () => markRead([ id ]) }, _('Mark as Read'))
		]) : ''
	]);
}

function errorMessage(data) {
	if (!data || data.ok !== false)
		return '';
	return E('div', { 'class': 'alert-message warning', 'style': 'margin-top:1em' }, data.error || data.message || _('Unable to load notifications.'));
}

return view.extend({
	load() {
		return callBitsxl([ 'json', 'notifications' ]);
	},

	render(data) {
		const items = notificationList(data);
		const ids = unreadIds(items);
		const unread = items.filter((item) => !notificationRead(item)).length;

		return E('div', { 'class': 'cbi-map' }, [
			E('div', { 'style': 'display:flex;justify-content:space-between;gap:1em;align-items:center;flex-wrap:wrap' }, [
				E('div', {}, [
					E('h2', { 'style': 'margin:0' }, _('Notifikasi')),
					E('div', { 'style': 'margin-top:.25em;color:inherit;opacity:.65' }, _('Total') + ': ' + items.length + ' · ' + _('Unread') + ': ' + unread)
				]),
				E('div', { 'style': 'display:flex;gap:.45em;flex-wrap:wrap' }, [
					ids.length ? E('button', { 'class': 'btn cbi-button cbi-button-action', 'click': () => markRead(ids) }, _('Read All Unread')) : '',
					E('button', { 'class': 'btn cbi-button cbi-button-reload', 'click': () => window.location.reload() }, _('Refresh'))
				])
			]),
			errorMessage(data),
			items.length ? E('div', {}, items.map(notificationCard)) : (data && data.ok === false ? '' : E('div', { 'class': 'alert-message', 'style': 'margin-top:1em' }, _('No notifications available.')))
		]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
