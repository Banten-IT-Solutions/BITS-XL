'use strict';
'require view';
'require fs';
'require ui';

const BIN = '/usr/bin/bitsxl';
const SOFT_TRACK = 'rgba(127,127,127,.22)';
const SOFT_LINE = 'rgba(127,127,127,.16)';
const SOFT_BORDER = 'linear-gradient(transparent,transparent) padding-box,linear-gradient(135deg,rgba(127,127,127,.24),rgba(127,127,127,.08),rgba(127,127,127,.18)) border-box';
const PAYMENT_LOG_KEY = 'bitsxl.payment.logs.v1';
const QUOTA_HISTORY_KEY = 'bitsxl.quota.history.v1';
const LINK_KEYS = [ 'link', 'url', 'payment_url', 'deeplink', 'deep_link', 'qris_url', 'qr_url', 'detail_url', 'detail_link' ];
const DETAIL_KEYS = [ 'qris', 'qris_code', 'qr_code', 'detail', 'code', 'trx_code', 'transaction_code', 'reference_id', 'payment_id' ];
const TX_KEYS = [ 'transaction_id', 'transaction_code', 'trx_code', 'reference_id', 'payment_id' ];

function callBitsxl(args) {
	return L.resolveDefault(fs.exec_direct(BIN, args, 'json'), { ok: false, error: _('Unable to execute bitsxl') });
}

function logText(value) {
	if (value == null)
		return '';
	if (typeof value === 'string')
		return value;
	try {
		return JSON.stringify(value, null, 2);
	} catch (err) {
		return String(value);
	}
}

function textValue(value) {
	if (value == null || value === '')
		return '';
	if (typeof value === 'object')
		return logText(value);
	return String(value);
}

function firstValue(obj, keys) {
	if (!obj || typeof obj !== 'object')
		return '';
	for (let index = 0; index < keys.length; index++) {
		const value = obj[keys[index]];
		if (value != null && value !== '')
			return value;
	}
	return '';
}

function findScalarKey(value, keys, depth) {
	if (!value || typeof value !== 'object' || depth > 5)
		return '';

	if (Array.isArray(value)) {
		for (let index = 0; index < value.length; index++) {
			const found = findScalarKey(value[index], keys, depth + 1);
			if (found)
				return found;
		}
		return '';
	}

	const own = firstValue(value, keys);
	if (own != null && own !== '' && typeof own !== 'object')
		return String(own);

	const names = Object.keys(value);
	for (let index = 0; index < names.length; index++) {
		const found = findScalarKey(value[names[index]], keys, depth + 1);
		if (found)
			return found;
	}

	return '';
}

function transactionId(item) {
	const direct = firstValue(item, TX_KEYS);
	if (direct != null && direct !== '' && typeof direct !== 'object')
		return String(direct);
	return findScalarKey(item, TX_KEYS, 0);
}

function findHistoryList(value, depth) {
	if (Array.isArray(value))
		return value;
	if (!value || typeof value !== 'object' || depth > 5)
		return [];

	const keys = [ 'list', 'transaction_history', 'history', 'transactions', 'pending_payment' ];
	for (let index = 0; index < keys.length; index++) {
		const found = findHistoryList(value[keys[index]], depth + 1);
		if (found.length)
			return found;
	}

	if (value.data) {
		const found = findHistoryList(value.data, depth + 1);
		if (found.length)
			return found;
	}

	if (value.response) {
		const found = findHistoryList(value.response, depth + 1);
		if (found.length)
			return found;
	}

	return [];
}

function formatMoney(value) {
	if (value == null || value === '')
		return '';
	if (typeof value === 'string')
		return value;
	const number = Number(value);
	if (isNaN(number))
		return String(value);
	return 'IDR ' + String(Math.round(number)).replace(/\B(?=(\d{3})+(?!\d))/g, '.');
}

function formatDate(value) {
	if (value == null || value === '')
		return '';
	if (typeof value === 'string')
		return value;

	const number = Number(value);
	if (!number || isNaN(number))
		return '';

	const ms = number > 100000000000 ? number : number * 1000;
	try {
		return new Date(ms).toLocaleString('id-ID', {
			year: 'numeric',
			month: 'long',
			day: '2-digit',
			hour: '2-digit',
			minute: '2-digit'
		});
	} catch (err) {
		return new Date(ms).toISOString();
	}
}

function statusColor(value) {
	value = String(value || '').toUpperCase();
	if (value.indexOf('SUCCESS') >= 0 || value.indexOf('FINISH') >= 0 || value.indexOf('PAID') >= 0 || value === '000')
		return '#238636';
	if (value.indexOf('PENDING') >= 0 || value.indexOf('PROCESS') >= 0)
		return '#9a6700';
	if (value.indexOf('FAIL') >= 0 || value.indexOf('ERROR') >= 0 || value.indexOf('REFUND') >= 0)
		return '#b42318';
	return 'rgba(127,127,127,.82)';
}

function chip(label, value) {
	value = textValue(value);
	if (!value)
		return '';
	return E('span', {
		'style': 'display:inline-flex;align-items:center;gap:.35em;max-width:100%;border:1px solid ' + SOFT_LINE + ';border-radius:4px;padding:.22em .5em;background:rgba(127,127,127,.08);color:' + statusColor(value)
	}, [
		E('span', { 'style': 'color:inherit;opacity:.72;font-size:.9em' }, label),
		E('strong', { 'style': 'min-width:0;overflow-wrap:anywhere' }, value)
	]);
}

function fieldRow(label, value) {
	value = textValue(value);
	if (!value)
		return '';
	return E('div', { 'style': 'display:grid;grid-template-columns:10em minmax(0,1fr);gap:.65em;padding:.28em 0;border-top:1px solid ' + SOFT_LINE }, [
		E('div', { 'style': 'color:inherit;opacity:.62' }, label),
		E('div', { 'style': 'min-width:0;overflow-wrap:anywhere;font-weight:500' }, value)
	]);
}

function isLink(value) {
	value = String(value || '');
	return /^[a-z][a-z0-9+.-]*:/i.test(value);
}

function collectKeyValues(value, keys, out, seen, depth) {
	if (!value || typeof value !== 'object' || depth > 5)
		return;

	if (Array.isArray(value)) {
		value.forEach((item) => collectKeyValues(item, keys, out, seen, depth + 1));
		return;
	}

	Object.keys(value).forEach((key) => {
		const item = value[key];
		if (keys.indexOf(key) >= 0 && item != null && item !== '') {
			const signature = key + ':' + logText(item);
			if (!seen[signature]) {
				seen[signature] = true;
				out.push([ key, item ]);
			}
		}
		if (item && typeof item === 'object')
			collectKeyValues(item, keys, out, seen, depth + 1);
	});
}

function keyLabel(key) {
	return String(key || '')
		.replace(/_/g, ' ')
		.replace(/\b\w/g, (char) => char.toUpperCase());
}

function linkPanel(item) {
	const links = [];
	const details = [];
	const seen = {};

	collectKeyValues(item, LINK_KEYS, links, seen, 0);
	collectKeyValues(item, DETAIL_KEYS, details, seen, 0);

	const linkNodes = links.filter((entry) => isLink(entry[1])).map((entry) => E('a', {
		'class': 'btn cbi-button cbi-button-save',
		'href': String(entry[1]),
		'target': '_blank',
		'rel': 'noopener',
		'style': 'margin:.25em .35em .25em 0;max-width:100%;overflow:hidden;text-overflow:ellipsis'
	}, keyLabel(entry[0])));

	const rawNodes = links.filter((entry) => !isLink(entry[1])).concat(details).map((entry) => E('details', { 'style': 'margin-top:.55em' }, [
		E('summary', { 'style': 'cursor:pointer;font-weight:650' }, keyLabel(entry[0])),
		E('pre', { 'style': 'margin-top:.45em;max-height:18em;overflow:auto;white-space:pre-wrap;word-break:break-word;background:rgba(127,127,127,.08);border:1px solid ' + SOFT_LINE + ';border-radius:6px;padding:.7em' }, logText(entry[1]))
	]));

	if (!linkNodes.length && !rawNodes.length)
		return '';

	return E('div', { 'style': 'margin-top:.8em' }, [
		linkNodes.length ? E('div', { 'style': 'display:flex;gap:.35em;flex-wrap:wrap' }, linkNodes) : '',
		...rawNodes
	]);
}

function showStatusModal(title, tx, data) {
	ui.showModal(title, [
		E('div', { 'style': 'font-weight:650;overflow-wrap:anywhere' }, tx),
		responsePanel(data),
		E('pre', { 'style': 'margin-top:.8em;max-height:30em;overflow:auto;white-space:pre-wrap;word-break:break-word;background:rgba(127,127,127,.08);border:1px solid ' + SOFT_LINE + ';border-radius:6px;padding:.75em' }, logText(data)),
		E('div', { 'style': 'display:flex;justify-content:flex-end;margin-top:1em' }, [
			E('button', { 'class': 'btn cbi-button', 'click': () => ui.hideModal() }, _('Close'))
		])
	]);
}

function refreshStatusButton(item) {
	const tx = transactionId(item);
	if (!tx)
		return '';

	return E('button', {
		'class': 'btn cbi-button cbi-button-action',
		'style': 'margin-top:.8em',
		'click': () => {
			ui.showModal(_('Transaction Status'), [
				E('div', { 'style': 'font-weight:650;overflow-wrap:anywhere' }, tx),
				E('div', { 'style': 'margin-top:.75em' }, _('Loading...'))
			]);
			callBitsxl([ 'json', 'transaction-status', tx ]).then((data) => showStatusModal(_('Transaction Status'), tx, data));
		}
	}, _('Refresh Status'));
}

function showPendingTransactions() {
	ui.showModal(_('Pending Transactions'), [ E('div', {}, _('Loading...')) ]);
	callBitsxl([ 'json', 'pending' ]).then((data) => showStatusModal(_('Pending Transactions'), _('pending_payment'), data));
}

function transactionCard(item, index) {
	item = item || {};
	const title = textValue(firstValue(item, [ 'title', 'package_name', 'name', 'payment_for' ])) || _('Transaction');
	const price = formatMoney(firstValue(item, [ 'price', 'raw_price', 'amount', 'total_amount' ]));
	const date = formatDate(firstValue(item, [ 'formated_date', 'formatted_date', 'date', 'created_at', 'timestamp' ]));
	const method = firstValue(item, [ 'payment_method_label', 'payment_with_label', 'payment_method', 'payment_with' ]);
	const status = firstValue(item, [ 'status', 'transaction_status' ]);
	const paymentStatus = firstValue(item, [ 'payment_status', 'payment_state' ]);
	const message = firstValue(item, [ 'error', 'status_message', 'message', 'description' ]);

	return E('div', { 'class': 'cbi-section', 'style': 'margin-top:1em;border:1px solid transparent;border-radius:8px;background:' + SOFT_BORDER + ';padding:1em' }, [
		E('div', { 'style': 'display:grid;grid-template-columns:2.5em minmax(0,1fr) auto;gap:.8em;align-items:start' }, [
			E('div', { 'style': 'width:2.5em;height:2.5em;line-height:2.5em;text-align:center;border-radius:4px;background:rgba(127,127,127,.16);font-weight:700' }, String(index + 1)),
			E('div', { 'style': 'min-width:0' }, [
				E('div', { 'style': 'font-size:1.08em;font-weight:700;line-height:1.25;overflow-wrap:anywhere' }, title),
				date ? E('div', { 'style': 'margin-top:.25em;color:inherit;opacity:.62' }, date) : ''
			]),
			price ? E('div', { 'style': 'font-weight:800;color:#0645c8;white-space:nowrap;text-align:right' }, price) : ''
		]),
		E('div', { 'style': 'display:flex;gap:.45em;flex-wrap:wrap;margin-top:.75em' }, [
			chip(_('Status'), status),
			chip(_('Payment'), paymentStatus)
		]),
		E('div', { 'style': 'margin-top:.75em' }, [
			fieldRow(_('Method'), method),
			fieldRow(_('Target'), firstValue(item, [ 'target_msisdn', 'msisdn', 'subscriber_id' ])),
			fieldRow(_('Validity'), item.validity),
			fieldRow(_('Category'), item.category),
			fieldRow(_('Message'), message)
		]),
		refreshStatusButton(item),
		linkPanel(item),
		E('details', { 'style': 'margin-top:.8em' }, [
			E('summary', { 'style': 'cursor:pointer;font-weight:650' }, _('Raw response')),
			E('pre', { 'style': 'margin-top:.55em;max-height:22em;overflow:auto;white-space:pre-wrap;word-break:break-word;background:rgba(127,127,127,.08);border:1px solid ' + SOFT_LINE + ';border-radius:6px;padding:.75em' }, logText(item))
		])
	]);
}

function responsePanel(data) {
	const status = firstValue(data, [ 'status', 'code' ]);
	const message = firstValue(data, [ 'error', 'message' ]);
	if (!status && !message)
		return '';
	return E('div', { 'class': data && data.ok === false ? 'alert-message warning' : 'alert-message', 'style': 'margin-top:1em' }, [
		status ? E('div', {}, [ E('strong', {}, _('Status')), ': ', textValue(status) ]) : '',
		message ? E('div', { 'style': 'margin-top:.25em;overflow-wrap:anywhere' }, [ E('strong', {}, _('Message')), ': ', textValue(message) ]) : ''
	]);
}

function readQuotaSnapshot() {
	const candidates = [];
	[ localStorage, sessionStorage ].forEach((storage) => {
		try {
			const value = JSON.parse(storage.getItem(QUOTA_HISTORY_KEY) || '{}');
			if (value && (Array.isArray(value.accounts) || Array.isArray(value.snapshots)))
				candidates.push(value);
		} catch (err) {}
	});
	const newest = (value) => {
		const snapshot = Array.isArray(value.snapshots) ? value.snapshots[0] : value;
		return Number(snapshot && snapshot.checked_at || 0);
	};
	candidates.sort((left, right) => newest(right) - newest(left));
	return candidates[0] || null;
}

function quotaSnapshots(data) {
	if (data && Array.isArray(data.snapshots))
		return data.snapshots.filter((item) => item && Array.isArray(item.accounts));
	if (data && data.history && Array.isArray(data.history.snapshots))
		return data.history.snapshots.filter((item) => item && Array.isArray(item.accounts));
	const single = data && data.snapshot && Array.isArray(data.snapshot.accounts) ? data.snapshot : (data && Array.isArray(data.accounts) ? data : null);
	return single ? [ single ] : [];
}

function quotaSnapshot(data) {
	const snapshots = quotaSnapshots(data);
	return snapshots[0] || null;
}

function quotaResponsePayload(response) {
	let payload = response && response.quota ? response.quota : response || {};
	if (payload.response)
		payload = payload.response;
	if (payload.data)
		payload = payload.data;
	return payload && typeof payload === 'object' ? payload : {};
}

function quotaPayload(entry) {
	return quotaResponsePayload(entry && entry.quota);
}

function compactQuotaResponse(response) {
	const payload = quotaResponsePayload(response);
	const packages = Array.isArray(payload.quotas) ? payload.quotas : [];
	return {
		quotas: packages.map((packageItem) => ({
			name: packageItem.name || '',
			group_name: packageItem.group_name || '',
			product_domain: packageItem.product_domain || packageItem.domain || '',
			product_subscription_type: packageItem.product_subscription_type || packageItem.subtype || '',
			parent_code: packageItem.parent_code || '',
			is_addon: packageItem.is_addon === true,
			benefits: (Array.isArray(packageItem.benefits) ? packageItem.benefits : []).map((benefit) => ({
				name: benefit.name || '',
				information: benefit.information || '',
				data_type: benefit.data_type || '',
				total: Number(benefit.total || 0),
				remaining: Number(benefit.remaining || 0),
				is_unlimited: benefit.is_unlimited === true,
				benefit_type: benefit.benefit_type || '',
				benefit_category: benefit.benefit_category || ''
			}))
		}))
	};
}

function quotaResponseOk(response) {
	const payload = quotaResponsePayload(response);
	const status = String(response && response.status || payload.status || '').toUpperCase();
	const code = String(response && response.code || payload.code || '').toUpperCase();
	const success = status === 'SUCCESS' || code === '000' || (!status && !code && response && response.ok === true);
	return !!response && response.ok !== false && success && Array.isArray(payload.quotas);
}

function saveQuotaSnapshot(snapshot) {
	const history = {
		schema_version: 1,
		snapshots: [ snapshot ]
	};
	const text = JSON.stringify(history);
	try {
		localStorage.setItem(QUOTA_HISTORY_KEY, text);
		try {
			sessionStorage.removeItem(QUOTA_HISTORY_KEY);
		} catch (err) {}
		return true;
	} catch (err) {
		try {
			localStorage.removeItem(QUOTA_HISTORY_KEY);
		} catch (removeError) {}
		try {
			sessionStorage.setItem(QUOTA_HISTORY_KEY, text);
			return true;
		} catch (sessionError) {
			return false;
		}
	}
}

function recheckAllQuota(button) {
	button.disabled = true;
	button.textContent = _('Loading accounts...');
	return callBitsxl([ 'json', 'accounts' ]).then((accountsResult) => {
		const accounts = accountsResult && Array.isArray(accountsResult.accounts) ? accountsResult.accounts : [];
		if (!accounts.length) {
			button.disabled = false;
			button.textContent = _('Recheck');
			ui.addNotification(null, E('p', {}, accountsResult && (accountsResult.error || accountsResult.message) || _('No registered number found.')), 'warning');
			return;
		}
		const snapshot = {
			schema_version: 1,
			checked_at: Math.floor(Date.now() / 1000),
			ok: false,
			complete: false,
			total_accounts: accounts.length,
			succeeded: 0,
			failed: 0,
			accounts: []
		};
		let chain = Promise.resolve();
		accounts.forEach((account, index) => {
			chain = chain.then(() => {
				button.textContent = _('Checking %d/%d...').format(index + 1, accounts.length);
				return callBitsxl([ 'json', 'quota', account.number, 'fresh' ]).then((response) => {
					const ok = quotaResponseOk(response);
					const entry = {
						number: account.number,
						subscription_type: account.subscription_type || '',
						ok: ok
					};
					if (ok) {
						entry.quota = compactQuotaResponse(response);
						snapshot.succeeded++;
					} else {
						entry.error = response && (response.error || response.message || response.code) || _('Quota check failed.');
						snapshot.failed++;
					}
					snapshot.accounts.push(entry);
				});
			});
		});
		return chain.then(() => {
			button.disabled = false;
			button.textContent = _('Recheck');
			snapshot.ok = snapshot.succeeded > 0;
			snapshot.complete = snapshot.failed === 0;
			if (!saveQuotaSnapshot(snapshot)) {
				ui.addNotification(null, E('p', {}, _('Fresh quota result could not be saved in this browser.')), 'warning');
				return;
			}
			ui.addNotification(null, E('p', {}, _('%d number checked, %d failed.').format(snapshot.succeeded, snapshot.failed)), snapshot.failed ? 'warning' : 'info');
			window.location.href = window.location.pathname + window.location.search;
		});
	});
}

function formatBytes(value) {
	value = Math.max(0, Number(value || 0));
	const units = [ 'B', 'KB', 'MB', 'GB', 'TB' ];
	let index = 0;
	while (value >= 1024 && index < units.length - 1) {
		value /= 1024;
		index++;
	}
	const digits = index === 0 || value >= 10 ? 0 : 2;
	return '%s %s'.format(value.toFixed(digits), units[index]);
}

function quotaPercent(remaining, total) {
	remaining = Number(remaining || 0);
	total = Number(total || 0);
	return total > 0 ? Math.max(0, Math.min(100, Math.round(remaining * 100 / total))) : 0;
}

function metadataAddon(packageItem, benefit) {
	if (packageItem && (packageItem.is_addon === true || packageItem.parent_code))
		return true;
	const type = String(benefit && benefit.benefit_type || '').trim().toUpperCase();
	const category = String(benefit && benefit.benefit_category || '').trim().toUpperCase();
	const general = [ '', 'DEFAULT', 'GENERAL', 'REGULAR', 'MAIN', 'BASIC', 'DATA', 'INTERNET' ];
	if (general.indexOf(type) < 0 || general.indexOf(category) < 0)
		return true;
	const text = [
		packageItem && packageItem.name,
		packageItem && packageItem.group_name,
		benefit && benefit.name,
		benefit && benefit.information
	].join(' ').toUpperCase();
	const markers = [ 'YOUTUBE', 'WHATSAPP', 'TIKTOK', 'INSTAGRAM', 'FACEBOOK', 'NETFLIX', 'SPOTIFY', 'VIDIO', 'ZOOM', 'SOCIAL MEDIA', 'MUSIC', 'GAME', 'APLIKASI', 'APPLICATION', 'ADD-ON', 'ADDON' ];
	return markers.some((marker) => text.indexOf(marker) >= 0);
}

function accountQuotaBenefits(entry) {
	const payload = quotaPayload(entry);
	const packages = Array.isArray(payload.quotas) ? payload.quotas : [];
	const result = [];
	packages.forEach((packageItem) => {
		(Array.isArray(packageItem.benefits) ? packageItem.benefits : []).forEach((benefit) => {
			if (String(benefit.data_type || '').toUpperCase() !== 'DATA')
				return;
			result.push({
				name: benefit.name || _('Data quota'),
				package_name: packageItem.name || '',
				group_name: packageItem.group_name || '',
				remaining: Math.max(0, Number(benefit.remaining || 0)),
				total: Math.max(0, Number(benefit.total || 0)),
				unlimited: benefit.is_unlimited === true,
				addon: metadataAddon(packageItem, benefit)
			});
		});
	});
	return result;
}

function sumQuotaBenefits(items) {
	return (items || []).reduce((sum, item) => {
		if (item.unlimited)
			sum.unlimited++;
		else {
			sum.remaining += Number(item.remaining || 0);
			sum.total += Number(item.total || 0);
		}
		return sum;
	}, { remaining: 0, total: 0, unlimited: 0 });
}

function quotaProgress(summary, height) {
	const percent = quotaPercent(summary.remaining, summary.total);
	const color = percent >= 60 ? '#46a546' : (percent >= 30 ? '#c09853' : '#b94a48');
	const filled = Math.round(percent / 10);
	const cells = [];
	for (let index = 0; index < 10; index++) {
		cells.push(E('span', {
			'style': 'display:block;flex:1;height:100%;border-radius:2px;background:' + (index < filled ? color : SOFT_TRACK)
		}));
	}
	return E('div', {
		'role': 'progressbar',
		'aria-valuemin': '0',
		'aria-valuemax': '100',
		'aria-valuenow': String(percent),
		'style': 'display:flex;gap:3px;height:' + (height || '13px') + ';align-items:stretch'
	}, cells);
}

function quotaBenefitRow(item) {
	const summary = { remaining: item.remaining, total: item.total };
	return E('div', { 'style': 'padding:.75em 0;border-top:1px solid ' + SOFT_LINE }, [
		E('div', { 'style': 'display:grid;grid-template-columns:minmax(0,1fr) auto;gap:.4em 1em;align-items:start;margin-bottom:.45em' }, [
			E('div', { 'style': 'min-width:0' }, [
				E('div', { 'style': 'font-weight:650;line-height:1.25;overflow-wrap:anywhere' }, item.name),
				E('div', { 'style': 'margin-top:.18em;color:inherit;opacity:.55;font-size:.88em;overflow-wrap:anywhere' }, [ item.package_name, item.group_name ].filter(Boolean).join(' · '))
			]),
			E('div', { 'style': 'text-align:right;white-space:nowrap;font-weight:700;color:#0645c8' }, item.unlimited ? _('Unlimited') : '%s / %s'.format(formatBytes(item.remaining), formatBytes(item.total)))
		]),
		item.unlimited ? '' : quotaProgress(summary, '13px')
	]);
}

function quotaGroup(title, items) {
	if (!items.length)
		return '';
	const summary = sumQuotaBenefits(items);
	return E('div', { 'style': 'margin-top:.9em;padding:.85em;border:1px solid ' + SOFT_LINE + ';border-radius:8px;background:rgba(127,127,127,.035)' }, [
		E('div', { 'style': 'display:flex;justify-content:space-between;gap:1em;align-items:center;margin-bottom:.15em' }, [
			E('div', { 'style': 'font-weight:750' }, title),
			E('div', { 'style': 'white-space:nowrap;font-size:.92em;color:inherit;opacity:.68' }, summary.unlimited ? _('%s + %d unlimited').format(formatBytes(summary.remaining), summary.unlimited) : formatBytes(summary.remaining))
		]),
		...items.map(quotaBenefitRow)
	]);
}

function quotaAccountCard(entry) {
	if (!entry || entry.ok === false) {
		return E('div', { 'class': 'cbi-section', 'style': 'margin-top:1em;border:1px solid rgba(185,74,72,.35);border-radius:10px;padding:1em' }, [
			E('div', { 'style': 'font-weight:750' }, entry && entry.number || _('Unknown number')),
			E('div', { 'class': 'alert-message warning', 'style': 'margin-top:.7em' }, entry && entry.error || _('Quota check failed.'))
		]);
	}
	const benefits = accountQuotaBenefits(entry);
	const main = benefits.filter((item) => !item.addon);
	const addons = benefits.filter((item) => item.addon);
	const summary = sumQuotaBenefits(benefits);
	return E('div', { 'class': 'cbi-section', 'style': 'margin-top:1em;border:1px solid transparent;border-radius:10px;background:' + SOFT_BORDER + ';padding:1em' }, [
		E('div', { 'style': 'display:grid;grid-template-columns:minmax(0,1fr) auto;gap:.55em 1em;align-items:start' }, [
			E('div', { 'style': 'min-width:0' }, [
				E('div', { 'style': 'font-size:1.1em;font-weight:800;overflow-wrap:anywhere' }, entry.number || '-'),
				E('div', { 'style': 'margin-top:.2em;color:inherit;opacity:.58;font-size:.9em' }, entry.subscription_type || '')
			]),
			E('div', { 'style': 'text-align:right;white-space:nowrap' }, [
				E('div', { 'style': 'font-size:1.1em;font-weight:800;color:#0645c8' }, formatBytes(summary.remaining)),
				E('div', { 'style': 'margin-top:.15em;color:inherit;opacity:.58;font-size:.88em' }, _('%d%% remaining').format(quotaPercent(summary.remaining, summary.total)))
			])
		]),
		benefits.length ? quotaGroup(_('Kuota Utama'), main) : E('div', { 'class': 'alert-message warning', 'style': 'margin-top:.8em' }, _('No data quota detail found.')),
		quotaGroup(_('Kuota Aplikasi / Add-on'), addons)
	]);
}

function quotaHistoryPage(data) {
	const snapshot = quotaSnapshot(data);
	if (!snapshot) {
		return E('div', { 'class': 'cbi-map' }, [
			E('h2', {}, _('Kuota History')),
			E('div', { 'class': 'alert-message warning' }, (data && (data.error || data.message)) || _('No quota snapshot yet. Use Check All Number Kuota from the account popup.'))
		]);
	}
	const accounts = snapshot.accounts || [];
	const successful = accounts.filter((entry) => entry && entry.ok !== false);
	const totals = sumQuotaBenefits([].concat.apply([], successful.map(accountQuotaBenefits)));
	const succeeded = snapshot.succeeded != null ? Number(snapshot.succeeded) : successful.length;
	const failed = snapshot.failed != null ? Number(snapshot.failed) : Math.max(0, accounts.length - succeeded);
	const recheck = E('button', {
		'class': 'btn cbi-button cbi-button-apply bitsxl-quota-history-recheck',
		'click': () => recheckAllQuota(recheck)
	}, _('Recheck'));
	return E('div', { 'class': 'cbi-map' }, [
		E('style', {}, [
			'.bitsxl-quota-history-actions{display:flex;align-items:center;min-width:0;max-width:100%}',
			'.bitsxl-quota-history-actions .bitsxl-quota-history-recheck{width:auto;white-space:nowrap}'
		].join('')),
		E('div', { 'style': 'display:flex;justify-content:space-between;gap:1em;align-items:center;flex-wrap:wrap' }, [
			E('div', {}, [
				E('h2', { 'style': 'margin:0' }, _('Kuota History')),
				E('div', { 'style': 'margin-top:.25em;color:inherit;opacity:.62' }, snapshot.checked_at ? formatDate(snapshot.checked_at) : '')
			]),
			E('div', { 'class': 'bitsxl-quota-history-actions' }, [
				recheck
			])
		]),
		E('div', { 'class': 'cbi-section', 'style': 'margin-top:1em;padding:clamp(1em,4vw,1.5em);border:1px solid transparent;border-radius:8px;background:' + SOFT_BORDER }, [
			E('div', { 'style': 'font-size:.9em;color:inherit;opacity:.58;font-weight:600' }, _('Total Sisa Kuota')),
			E('div', { 'style': 'display:flex;justify-content:space-between;gap:1em;align-items:flex-end;flex-wrap:wrap;margin:.45em 0 .8em' }, [
				E('div', { 'style': 'font-size:clamp(2em,8vw,3.2em);font-weight:850;line-height:1' }, formatBytes(totals.remaining)),
				E('div', { 'style': 'text-align:right;color:inherit;opacity:.68' }, [
					E('div', { 'style': 'font-weight:700' }, _('of %s total').format(formatBytes(totals.total))),
					totals.unlimited ? E('div', { 'style': 'margin-top:.2em' }, _('%d unlimited quota').format(totals.unlimited)) : ''
				])
			]),
			quotaProgress(totals, '16px'),
			E('div', { 'style': 'display:flex;gap:.6em;flex-wrap:wrap;margin-top:.85em;font-size:.9em' }, [
				E('span', { 'style': 'padding:.25em .6em;border:1px solid ' + SOFT_LINE + ';border-radius:4px;background:rgba(127,127,127,.08)' }, _('%d number success').format(succeeded)),
				E('span', { 'style': 'padding:.25em .6em;border:1px solid ' + SOFT_LINE + ';border-radius:4px;background:rgba(127,127,127,.08)' }, _('%d failed').format(failed)),
				E('span', { 'style': 'padding:.25em .6em;border:1px solid ' + SOFT_LINE + ';border-radius:4px;background:rgba(127,127,127,.08)' }, _('%d%% remaining').format(quotaPercent(totals.remaining, totals.total)))
			])
		]),
		failed ? E('div', { 'class': 'alert-message warning', 'style': 'margin-top:1em' }, _('Some numbers could not be checked. Their session may need a new OTP.')) : '',
		E('h3', { 'style': 'margin:1.25em 0 .25em' }, _('Detail per Nomor')),
		...accounts.map(quotaAccountCard),
		E('button', { 'class': 'btn cbi-button cbi-button-reload', 'style': 'margin-top:1em', 'click': () => window.location.reload() }, _('Refresh'))
	]);
}

function currentMode() {
	const path = window.location.pathname + window.location.search + window.location.hash;
	if (path.indexOf('/riwayat/quota-history') >= 0 || path.indexOf('riwayat/quota-history') >= 0)
		return 'quota-history';
	return path.indexOf('/riwayat/logs') >= 0 || path.indexOf('riwayat/logs') >= 0 ? 'logs' : 'transaction-history';
}

function readPaymentLogs() {
	if (typeof localStorage === 'undefined')
		return [];
	try {
		const logs = JSON.parse(localStorage.getItem(PAYMENT_LOG_KEY) || '[]');
		return Array.isArray(logs) ? logs : [];
	} catch (err) {
		return [];
	}
}

function paymentModeLabel(value) {
	if (value === 'balance-decoy-standard' || value === 'decoy-standard')
		return _('Balance + Decoy');
	if (value === 'balance-decoy-v2' || value === 'decoy-v2' || value === 'balance-decoy' || value === 'decoy' || value === 'prio')
		return _('Balance + Decoy V2');
	return value;
}

function paymentLogCard(entry, index) {
	entry = entry || {};
	const response = entry.response || entry.payload || entry;
	const status = entry.status || firstValue(response, [ 'status', 'payment_status', 'code' ]);
	const message = entry.message || firstValue(response, [ 'message', 'error', 'description', 'title', 'code_detail' ]);
	const items = Array.isArray(entry.items) ? entry.items : [];
	const date = formatDate(entry.time || entry.timestamp || entry.created_at);
	const quotedTotal = entry.quoted_total != null && entry.quoted_total !== '' ? entry.quoted_total : (items.length ? items.reduce((sum, item) => sum + Number(item.price || 0), 0) : '');
	const totalAmount = entry.total_amount != null && entry.total_amount !== '' ? entry.total_amount : firstValue(response, [ 'total_amount' ]);
	const customPrice = entry.custom_price != null && entry.custom_price !== '' ? entry.custom_price : firstValue(response, [ 'custom_price' ]);

	return E('div', { 'class': 'cbi-section', 'style': 'margin-top:1em;border:1px solid transparent;border-radius:8px;background:' + SOFT_BORDER + ';padding:1em' }, [
		E('div', { 'style': 'display:grid;grid-template-columns:2.5em minmax(0,1fr) auto;gap:.8em;align-items:start' }, [
			E('div', { 'style': 'width:2.5em;height:2.5em;line-height:2.5em;text-align:center;border-radius:4px;background:rgba(127,127,127,.16);font-weight:700' }, String(index + 1)),
			E('div', { 'style': 'min-width:0' }, [
				E('div', { 'style': 'font-size:1.08em;font-weight:700;line-height:1.25;overflow-wrap:anywhere' }, message || _('Server payment log')),
				date ? E('div', { 'style': 'margin-top:.25em;color:inherit;opacity:.62' }, date) : ''
			]),
			chip(_('Status'), status)
		]),
		E('div', { 'style': 'display:flex;gap:.45em;flex-wrap:wrap;margin-top:.75em' }, [
			chip(_('Source'), entry.source),
			chip(_('Payment'), paymentModeLabel(entry.payment)),
			chip(_('Quoted'), formatMoney(quotedTotal)),
			chip(_('Paid'), formatMoney(totalAmount)),
			chip(_('Custom'), formatMoney(customPrice))
		]),
		items.length ? E('div', { 'style': 'margin-top:.75em;border-top:1px solid ' + SOFT_LINE }, items.map((item) => E('div', { 'style': 'display:grid;grid-template-columns:minmax(0,1fr) auto;gap:1em;padding:.45em 0;border-bottom:1px solid ' + SOFT_LINE }, [
			E('div', { 'style': 'min-width:0;overflow-wrap:anywhere' }, item.name || item.code || '-'),
			E('div', { 'style': 'white-space:nowrap;font-weight:650;color:#0645c8' }, formatMoney(item.price))
		]))) : '',
		refreshStatusButton(response),
		linkPanel(response),
		E('details', { 'style': 'margin-top:.8em' }, [
			E('summary', { 'style': 'cursor:pointer;font-weight:650' }, _('Raw response')),
			E('pre', { 'style': 'margin-top:.55em;max-height:26em;overflow:auto;white-space:pre-wrap;word-break:break-word;background:rgba(127,127,127,.08);border:1px solid ' + SOFT_LINE + ';border-radius:6px;padding:.75em' }, logText(response))
		])
	]);
}

function transactionHistoryPage(data) {
	const history = findHistoryList(data, 0);

	return E('div', { 'class': 'cbi-map' }, [
		E('div', { 'style': 'display:flex;justify-content:space-between;gap:1em;align-items:center;flex-wrap:wrap' }, [
			E('div', {}, [
				E('h2', { 'style': 'margin:0' }, _('Riwayat')),
				E('div', { 'style': 'margin-top:.25em;color:inherit;opacity:.65' }, _('Transaction History'))
			]),
			E('div', { 'style': 'display:flex;gap:.45em;flex-wrap:wrap' }, [
				E('button', { 'class': 'btn cbi-button cbi-button-neutral', 'click': showPendingTransactions }, _('Pending Payments')),
				E('button', { 'class': 'btn cbi-button cbi-button-reload', 'click': () => window.location.reload() }, _('Refresh'))
			])
		]),
		responsePanel(data),
		history.length ? E('div', {}, history.map(transactionCard)) : E('div', { 'class': 'alert-message warning', 'style': 'margin-top:1em' }, (data && (data.error || data.message)) || _('No transaction history.'))
	]);
}

function paymentLogsPage() {
	const logs = readPaymentLogs();

	return E('div', { 'class': 'cbi-map' }, [
		E('div', { 'style': 'display:flex;justify-content:space-between;gap:1em;align-items:center;flex-wrap:wrap' }, [
			E('div', {}, [
				E('h2', { 'style': 'margin:0' }, _('Riwayat')),
				E('div', { 'style': 'margin-top:.25em;color:inherit;opacity:.65' }, _('Logs'))
			]),
			E('button', { 'class': 'btn cbi-button cbi-button-reload', 'click': () => window.location.reload() }, _('Refresh'))
		]),
		logs.length ? E('div', {}, logs.map(paymentLogCard)) : E('div', { 'class': 'alert-message warning', 'style': 'margin-top:1em' }, _('No payment logs.'))
	]);
}

return view.extend({
	load() {
		const mode = currentMode();
		if (mode === 'logs')
			return Promise.resolve({});
		if (mode === 'quota-history') {
			const local = readQuotaSnapshot();
			return Promise.resolve(local || {});
		}
		return callBitsxl([ 'json', 'transaction-history' ]);
	},

	render(data) {
		const mode = currentMode();
		return mode === 'logs' ? paymentLogsPage() : (mode === 'quota-history' ? quotaHistoryPage(data) : transactionHistoryPage(data));
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
