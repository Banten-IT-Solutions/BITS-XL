'use strict';
'require view';
'require fs';
'require ui';

const BIN = '/usr/bin/bitsxl';
const SOFT_TRACK = 'rgba(127,127,127,.22)';
const SOFT_LINE = 'rgba(127,127,127,.16)';
const SOFT_BORDER = 'linear-gradient(transparent,transparent) padding-box,linear-gradient(135deg,rgba(127,127,127,.26),rgba(127,127,127,.08),rgba(127,127,127,.20)) border-box';
const BUY_STYLE = 'background:#003b95;border-color:#003b95;color:#fff';
const PAYMENT_LOG_KEY = 'bitsxl.payment.logs.v1';
const QUOTA_HISTORY_KEY = 'bitsxl.quota.history.v1';
const PAYMENT_MODES = [
	{ value: 'balance', label: _('Balance'), command: 'balance' },
	{ value: 'balance-decoy-standard', label: _('Balance + Decoy'), command: 'balance-decoy-standard' },
	{ value: 'balance-decoy-v2', label: _('Balance + Decoy V2'), command: 'balance-decoy-v2' },
	{ value: 'qris', label: _('QRIS'), command: 'qris' },
	{ value: 'dana', label: _('DANA'), command: 'dana', ewallet: true, wallet: true },
	{ value: 'shopeepay', label: _('ShopeePay'), command: 'shopeepay', ewallet: true },
	{ value: 'gopay', label: _('GoPay'), command: 'gopay', ewallet: true },
	{ value: 'point', label: _('Point'), command: 'point', single: true },
	{ value: 'voucher', label: _('Voucher'), command: 'voucher', single: true },
	{ value: 'gift', label: _('Gift/Send Bonus'), command: 'gift', destination: true, single: true }
];
let transactionHistoryExpanded = false;
let transactionHistoryLoading = false;
let transactionHistoryLoaded = false;
let transactionHistoryResult = null;
let transactionHistoryError = '';
let tieringLoading = false;
let tieringLoaded = false;
let tieringResult = null;
let tieringError = '';

function callBitsxl(args) {
	return L.resolveDefault(fs.exec_direct(BIN, args, 'json'), { ok: false, error: _('Unable to execute bitsxl') });
}

function notifyResult(res, okText) {
	if (res && res.ok)
		ui.addNotification(null, E('p', {}, okText || _('Done.')), 'info');
	else
		ui.addNotification(null, E('p', {}, (res && (res.error || res.message)) || _('Command failed.')), 'warning');
}

function pageTitle(title) {
	const main = document.getElementById('maincontent');
	const tabmenu = document.getElementById('tabmenu');
	if (!main || !tabmenu || main.querySelector('.bitsxl-page-title'))
		return;
	main.insertBefore(E('div', { 'class': 'bitsxl-page-title' }, [
		E('h2', { 'name': 'content', 'style': 'margin:0 0 .35em' }, title),
		E('div', { 'style': 'border-top:1px solid ' + SOFT_LINE + ';margin:0 0 1em' })
	]), tabmenu);
}

function copyText(text, ev) {
	if (ev) {
		ev.preventDefault();
		ev.stopPropagation();
	}
	text = String(text || '').trim();
	if (!text)
		return;
	const done = () => ui.addNotification(null, E('p', {}, _('Copied.')), 'info');
	const fallback = () => {
		const area = document.createElement('textarea');
		area.value = text;
		area.style.position = 'fixed';
		area.style.opacity = '0';
		document.body.appendChild(area);
		area.select();
		document.execCommand('copy');
		document.body.removeChild(area);
		done();
	};
	if (typeof navigator !== 'undefined' && navigator.clipboard && navigator.clipboard.writeText)
		return navigator.clipboard.writeText(text).then(done, fallback);
	fallback();
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

function logField(payload, keys) {
	const stack = [ payload, payload && payload.response, payload && payload.response && payload.response.response, payload && payload.response && payload.response.data ];
	for (let stackIndex = 0; stackIndex < stack.length; stackIndex++) {
		const item = stack[stackIndex];
		if (!item || typeof item !== 'object')
			continue;
		for (let index = 0; index < keys.length; index++)
			if (item[keys[index]] != null && item[keys[index]] !== '')
				return item[keys[index]];
	}
	return '';
}

function savePaymentLog(source, mode, items, payload, customAmount) {
	if (typeof localStorage === 'undefined')
		return;
	try {
		const logs = JSON.parse(localStorage.getItem(PAYMENT_LOG_KEY) || '[]');
		const status = logField(payload, [ 'status', 'payment_status', 'code' ]) || (payload && payload.ok ? 'SUCCESS' : 'FAILED');
		const message = logField(payload, [ 'message', 'error', 'description', 'title', 'code_detail' ]);
		const quotedTotal = (items || []).reduce((sum, item) => sum + Number(item.price || 0), 0);
		const totalAmount = logField(payload, [ 'total_amount' ]) || customAmount || '';
		const customPrice = logField(payload, [ 'custom_price' ]) || (customAmount && Number(customAmount) !== quotedTotal ? customAmount : '');
		logs.unshift({
			time: Date.now(),
			source: source,
			payment: mode,
			status: status,
			message: message,
			quoted_total: quotedTotal,
			total_amount: totalAmount,
			custom_price: customPrice,
			items: (items || []).map((item) => ({ name: item.name || '', code: item.code || '', price: item.price || 0 })),
			response: payload
		});
		localStorage.setItem(PAYMENT_LOG_KEY, JSON.stringify(logs.slice(0, 200)));
	} catch (err) {}
}

function paymentError(message, payload) {
	const err = new Error(message || _('Payment failed.'));
	err.payload = payload;
	return err;
}

function showPaymentError(err) {
	const payload = err && err.payload != null ? err.payload : err;
	ui.showModal(_('Payment Error'), [
		E('div', {}, [
			E('div', { 'class': 'alert-message warning' }, err && err.message || payload && (payload.error || payload.message) || _('Payment failed.')),
			E('pre', { 'style': 'margin-top:.75em;max-height:22em;overflow:auto;white-space:pre-wrap;word-break:break-word;background:rgba(127,127,127,.08);border:1px solid ' + SOFT_LINE + ';border-radius:6px;padding:.75em' }, logText(payload)),
			E('div', { 'style': 'display:flex;justify-content:flex-end;margin-top:1em' }, [
				E('button', { 'class': 'btn cbi-button', 'click': () => ui.hideModal() }, _('Close'))
			])
		])
	]);
}

function fmtBytes(value) {
	value = Number(value || 0);
	const units = [ 'B', 'KB', 'MB', 'GB', 'TB' ];
	let index = 0;
	while (value >= 1024 && index < units.length - 1) {
		value /= 1024;
		index++;
	}
	return '%s %s'.format(value >= 10 || index === 0 ? value.toFixed(0) : value.toFixed(2), units[index]);
}

function fmtQuota(value, type) {
	type = String(type || '').toUpperCase();
	if (type === 'VOICE')
		return '%.2f %s'.format(Number(value || 0) / 60, _('minutes'));
	if (type === 'TEXT')
		return '%d SMS'.format(Number(value || 0));
	return fmtBytes(value);
}

function fmtDate(epoch) {
	epoch = Number(epoch || 0);
	if (!epoch)
		return 'N/A';
	return new Date(epoch * 1000).toISOString().slice(0, 10);
}

function fmtRupiah(value) {
	if (value == null || value === '')
		return 'N/A';
	const string = String(value);
	const number = typeof value === 'number' ? value : (/^[0-9]+$/.test(string) ? Number(string) : NaN);
	if (isNaN(number))
		return string;
	return 'Rp %s'.format(String(Math.round(number)).replace(/\B(?=(\d{3})+(?!\d))/g, '.'));
}

function balanceValue(balance) {
	if (!balance || typeof balance !== 'object')
		return 'N/A';
	const value = balance.remaining != null ? balance.remaining : balance.balance && balance.balance.remaining;
	return fmtRupiah(value);
}

function balanceExpiry(balance) {
	if (!balance || typeof balance !== 'object')
		return 0;
	return balance.expired_at || balance.balance && balance.balance.expired_at || balance.grace_end_date || 0;
}

function balanceData(data) {
	const balance = data.balance || {};
	return balance.data || balance;
}

function creditData(balance) {
	if (!balance || typeof balance !== 'object')
		return null;
	const credit = balance.credit != null ? balance.credit : (balance.pulsa != null ? balance.pulsa : balance.balance_credit);
	if (credit != null && typeof credit !== 'object')
		return { remaining: credit };
	if (!credit || typeof credit !== 'object')
		return null;
	const nested = credit.balance && typeof credit.balance === 'object' ? credit.balance : {};
	const value = firstValue(credit, [ 'remaining', 'amount', 'value', 'raw_balance' ], firstValue(nested, [ 'remaining', 'amount', 'value', 'raw_balance' ], null));
	return value == null ? null : credit;
}

function quotaData(data) {
	const quota = data.quota || {};
	return quota.data || quota;
}

function payloadFailed(payload) {
	if (!payload || typeof payload !== 'object')
		return false;
	if (payload.ok === false || payload.error)
		return true;
	const status = payload.status != null ? String(payload.status).toUpperCase() : '';
	return (status && status !== 'SUCCESS') || String(payload.code || '') === '132';
}

function payloadErrorText(label, payload) {
	const payloads = [ payload, payload && payload.data ].filter((item) => item && typeof item === 'object');
	const keys = [ 'message', 'error', 'description', 'title', 'code_detail', 'code' ];
	for (let index = 0; index < payloads.length; index++) {
		const item = payloads[index];
		if (!payloadFailed(item))
			continue;
		const nested = item.response && typeof item.response === 'object' ? item.response : {};
		const message = firstValue(item, keys, firstValue(nested, keys, _('Failed')));
		const code = firstValue(item, [ 'code_detail', 'code' ], firstValue(nested, [ 'code_detail', 'code' ], ''));
		return '%s: %s%s'.format(label, message, code && String(code) !== String(message) ? ' (' + code + ')' : '');
	}
	return '';
}

function payloadErrorNotice(errors, payloads) {
	if (!errors.length)
		return '';
	return E('div', { 'class': 'alert-message warning' }, [
		E('div', {}, errors.join(' · ')),
		E('pre', { 'style': 'margin-top:.65em;max-height:18em;overflow:auto;white-space:pre-wrap;word-break:break-word;background:rgba(127,127,127,.08);border:1px solid ' + SOFT_LINE + ';border-radius:6px;padding:.65em' }, logText(payloads))
	]);
}

function optionalPayload(res, keys) {
	if (!res || res.ok === false || res.error)
		return null;
	if (res.status && String(res.status).toUpperCase() !== 'SUCCESS')
		return null;
	for (let index = 0; index < keys.length; index++)
		if (res[keys[index]]) {
			const payload = res[keys[index]];
			return payload && payload.status && String(payload.status).toUpperCase() !== 'SUCCESS' ? null : payload;
		}
	const payload = res.data || res;
	return payload && payload.status && String(payload.status).toUpperCase() !== 'SUCCESS' ? null : payload;
}

function tieringData(res) {
	let payload = optionalPayload(res, [ 'tiering', 'tiering_info', 'response' ]);
	if (!payload)
		return null;
	payload = payload.data || payload;
	const tier = firstValue(payload, [ 'tier', 'tier_name', 'current_tier', 'tier_level' ], '');
	const point = firstValue(payload, [ 'current_point', 'current_points', 'point', 'points', 'total_point' ], '');
	return tier === '' && point === '' ? null : { tier: tier, point: point };
}

function transactionHistoryData(res) {
	let payload = optionalPayload(res, [ 'transaction_history', 'history', 'response' ]);
	if (!payload)
		return [];
	payload = payload.data || payload;
	if (Array.isArray(payload))
		return payload;
	if (Array.isArray(payload.list))
		return payload.list;
	if (Array.isArray(payload.transactions))
		return payload.transactions;
	if (Array.isArray(payload.pending_payment))
		return payload.pending_payment;
	return [];
}

function storeSegments(data) {
	const response = data.store_segments || data || {};
	if (Array.isArray(response))
		return response;
	const payload = response.data || response;
	if (Array.isArray(payload))
		return payload;
	return Array.isArray(payload.store_segments) ? payload.store_segments : [];
}

function pct(rem, total) {
	rem = Number(rem || 0);
	total = Number(total || 0);
	if (!total)
		return 0;
	return Math.max(0, Math.min(100, Math.round(rem * 100 / total)));
}

function progressColor(percent) {
	return percent >= 60 ? '#46a546' : (percent >= 30 ? '#c09853' : '#b94a48');
}

function progressBar(percent, height) {
	percent = Math.max(0, Math.min(100, Number(percent || 0)));
	const color = progressColor(percent);
	const segments = 10;
	const filled = Math.round(percent / 10);
	const cells = [];
	for (let index = 0; index < segments; index++) {
		cells.push(E('span', {
			'style': 'display:block;flex:1;height:100%;border-radius:2px;background:' + (index < filled ? color : SOFT_TRACK)
		}));
	}
	return E('div', { 'style': 'display:flex;gap:3px;height:' + (height || '12px') + ';align-items:stretch' }, cells);
}

function benefitCard(benefit) {
	const percent = pct(benefit.remaining, benefit.total);
	const quotaText = '%s / %s'.format(fmtQuota(benefit.remaining, benefit.data_type), fmtQuota(benefit.total, benefit.data_type));
	return E('div', { 'style': 'padding:.8em 0;border-top:1px solid ' + SOFT_LINE }, [
		E('div', { 'style': 'display:flex;justify-content:space-between;gap:.75em;align-items:flex-start;margin-bottom:.45em' }, [
			E('div', { 'style': 'font-weight:600;min-width:0' }, benefit.name || 'benefit'),
			E('div', { 'style': 'white-space:nowrap;color:inherit;opacity:.6;font-weight:400;text-align:right' }, quotaText)
		]),
		progressBar(percent, '13px')
	]);
}

function trashIcon() {
	return E('span', { 'aria-hidden': 'true', 'style': 'display:inline-block;position:relative;width:16px;height:16px;color:#d9534f;vertical-align:-3px' }, [
		E('span', { 'style': 'position:absolute;left:3px;top:0;width:10px;height:5px;border:2px solid currentColor;border-bottom:0;border-radius:5px 5px 0 0;box-sizing:border-box' }),
		E('span', { 'style': 'position:absolute;left:1px;top:5px;width:14px;height:4px;background:currentColor;border-radius:4px 4px 1px 1px' }),
		E('span', { 'style': 'position:absolute;left:3px;top:9px;width:10px;height:7px;background:currentColor;clip-path:polygon(0 0,100% 0,84% 100%,16% 100%)' })
	]);
}


function packageDeleteButton(pkg) {
	const code = pkg.quota_code || pkg.code || '';
	const subtype = pkg.product_subscription_type || pkg.subtype || '';
	const domain = pkg.product_domain || pkg.domain || '';
	if (!code || pkg.is_unsubscribable === false)
		return '';
	return E('button', {
		'class': 'btn cbi-button cbi-button-remove',
		'title': _('Delete package'),
		'style': 'flex:0 0 auto;align-self:flex-start;display:inline-flex;align-items:center;justify-content:center;box-sizing:border-box;width:2.35em;height:2.35em;min-width:0;padding:0;line-height:1;background:transparent;border-color:rgba(217,83,79,.55);box-shadow:none',
		'click': (ev) => {
			if (ev) {
				ev.preventDefault();
				ev.stopPropagation();
			}
			const name = pkg.name || _('this package');
			if (!confirm(_('Delete package %s?').format(name)))
				return;
			if (!confirm(_('This cannot be undone. Continue?')))
				return;
			return callBitsxl([ 'json', 'unsub', code, subtype, domain ]).then((res) => {
				notifyResult(res, _('Package delete request sent.'));
				if (res && res.ok)
					window.location.reload();
			});
		}
	}, trashIcon());
}

function nestedObject(object, key) {
	return object && object[key] && typeof object[key] === 'object' ? object[key] : {};
}

function formatDateValue(value) {
	if (value == null || value === '')
		return '';
	const number = Number(value);
	if (!isNaN(number) && number > 0)
		return fmtDate(number > 9999999999 ? number / 1000 : number);
	return String(value);
}

function meaningfulValue(value) {
	return value != null && value !== '' && value !== '-' && value !== 'N/A';
}

function packageInfoGrid(rows) {
	const nodes = [].concat.apply([], (rows || []).map((row) => [
		E('div', {}, row.label),
		E('div', { 'style': (row.strong ? 'font-weight:700;color:#0645c8;' : '') + 'overflow-wrap:anywhere' }, String(row.value == null || row.value === '' ? '-' : row.value))
	]));
	return E('div', { 'style': 'display:grid;grid-template-columns:auto minmax(0,1fr);gap:.45em 1em;margin-bottom:1em' }, nodes);
}

function packageDetailInfoRows(option, family, addon) {
	return [
		{ label: _('Price'), value: fmtMoney(option.price), strong: true },
		{ label: _('Validity'), value: option.validity || '-' },
		{ label: _('Point'), value: option.point == null ? '-' : String(option.point) },
		{ label: _('Payment For'), value: family.payment_for || '-' },
		{ label: _('Plan Type'), value: family.plan_type || '-' },
		{ label: _('Parent Code'), value: addon.parent_code || '-' }
	];
}

function packageDetailBody(detail, actionParam, options) {
	options = options || {};
	const option = detail.package_option || {};
	const family = detail.package_family || {};
	const variant = detail.package_detail_variant || {};
	const addon = detail.package_addon || {};
	const title = compactTitle([ family.name, variant.name, option.name ], actionParam);
	const tnc = cleanPackageText(option.tnc || option.description || '');
	const optionCode = option.package_option_code || actionParam;
	const familyCode = family.package_family_code || family.family_code || '';
	return E('div', {}, [
		E('div', { 'style': 'font-weight:700;font-size:1.18em;line-height:1.25;margin-bottom:.25em' }, title),
		packageCodeMeta(familyCode, optionCode, { copy: options.copyCode !== false }),
		E('div', { 'style': 'border-top:1px solid ' + SOFT_LINE + ';margin:.75em 0 .8em' }),
		packageInfoGrid(packageDetailInfoRows(option, family, addon)),
		E('div', { 'style': 'font-weight:700;margin:.2em 0 .45em' }, _('Quota Detail')),
		packageBenefitCards(option.benefits),
		tnc ? E('details', { 'style': 'margin-top:1em' }, [
			E('summary', { 'style': 'cursor:pointer;font-weight:650' }, _('Terms')),
			E('div', { 'style': 'white-space:pre-wrap;margin-top:.55em;color:inherit;opacity:.72;line-height:1.45' }, tnc)
		]) : '',
		options.footer || ''
	]);
}

function ownedPackageToPackageDetail(pkg) {
	const family = nestedObject(pkg, 'package_family');
	const variant = nestedObject(pkg, 'package_variants');
	const info = nestedObject(pkg, 'additional_benefit_info');
	return {
		package_family: {
			name: family.name || pkg.group_name || '',
			package_family_code: family.package_family_code || family.family_code || '',
			family_code: family.family_code || family.package_family_code || '',
			payment_for: family.payment_for || '',
			plan_type: family.plan_type || ''
		},
		package_detail_variant: {
			name: variant.name || variant.display_name || '',
			package_variant_code: variant.package_variant_code || ''
		},
		package_option: {
			name: pkg.name || '',
			package_option_code: firstValue(pkg, [ 'package_option_code', 'option_code', 'quota_code', 'code' ], ''),
			price: firstValue(pkg, [ 'price', 'discounted_price', 'original_price' ], ''),
			validity: firstValue(pkg, [ 'validity', 'validity_text' ], ''),
			point: firstValue(pkg, [ 'point', 'points' ], null),
			benefits: Array.isArray(pkg.benefits) ? pkg.benefits : [],
			description: info.content || ''
		},
		package_addon: {
			parent_code: pkg.parent_code || ''
		}
	};
}

function mergeOwnedPackageDetail(pkg, remoteDetail) {
	const local = ownedPackageToPackageDetail(pkg);
	if (!remoteDetail)
		return local;
	const option = remoteDetail.package_option || {};
	const family = remoteDetail.package_family || {};
	const variant = remoteDetail.package_detail_variant || {};
	const addon = remoteDetail.package_addon || {};
	return {
		package_family: Object.assign({}, local.package_family, family),
		package_detail_variant: Object.assign({}, local.package_detail_variant, variant),
		package_option: Object.assign({}, local.package_option, option, {
			benefits: local.package_option.benefits.length ? local.package_option.benefits : (Array.isArray(option.benefits) ? option.benefits : [])
		}),
		package_addon: Object.assign({}, local.package_addon, addon)
	};
}

function ownedPackageInfoRows(pkg, detail) {
	const option = detail.package_option || {};
	const family = detail.package_family || {};
	const price = Number(option.price || 0);
	const rows = [
		{ label: _('Group Name'), value: pkg.group_name },
		{ label: _('Price'), value: price > 0 ? fmtMoney(price) : '', strong: true },
		{ label: _('Validity'), value: option.validity },
		{ label: _('Payment For'), value: family.payment_for && family.payment_for !== 'BUY_PACKAGE' ? family.payment_for : '' },
		{ label: _('Plan Type'), value: family.plan_type && family.plan_type !== 'NORMAL' ? family.plan_type : '' },
		{ label: _('Active Date'), value: formatDateValue(pkg.active_date) },
		{ label: _('End Date'), value: formatDateValue(firstValue(pkg, [ 'end_date', 'expired_at' ], '')) },
		{ label: _('Recurring Date'), value: formatDateValue(pkg.recurring_date) },
		{ label: _('Recurring'), value: pkg.is_recurring == null ? '' : (pkg.is_recurring ? _('Yes') : _('No')) }
	];
	return rows.filter((row) => meaningfulValue(row.value));
}

function ownedPackageTechnicalRows(pkg) {
	return [
		{ label: _('Quota Code'), value: firstValue(pkg, [ 'quota_code', 'code' ], '') },
		{ label: _('Group Code'), value: pkg.group_code },
		{ label: _('Subscription Type'), value: firstValue(pkg, [ 'product_subscription_type', 'subtype' ], '') },
		{ label: _('Product Domain'), value: firstValue(pkg, [ 'product_domain', 'domain' ], '') }
	].filter((row) => meaningfulValue(row.value));
}

function ownedBenefitAmount(benefit) {
	if (benefit && benefit.is_unlimited)
		return _('Unlimited');
	return '%s / %s'.format(fmtQuota(benefit && benefit.remaining, benefit && benefit.data_type), fmtQuota(benefit && benefit.total, benefit && benefit.data_type));
}

function ownedBenefitRows(benefits) {
	if (!Array.isArray(benefits) || !benefits.length)
		return packageBenefitCards(benefits);
	return E('div', { 'style': 'margin-top:.45em;border-top:1px solid ' + SOFT_LINE }, benefits.map((benefit) => E('div', { 'style': 'padding:.75em 0;border-bottom:1px solid ' + SOFT_LINE }, [
		E('div', { 'style': 'display:grid;grid-template-columns:minmax(0,1fr) auto;gap:.5em 1em;align-items:flex-start;margin-bottom:.45em' }, [
			E('div', { 'style': 'min-width:0' }, [
				E('div', { 'style': 'font-weight:650;line-height:1.25' }, benefit.name || _('Benefit')),
				E('div', { 'style': 'color:inherit;opacity:.55;font-size:.9em;margin-top:.2em' }, [
					benefit.data_type || 'DATA',
					benefit.id ? ' · ' + benefit.id : ''
				])
			]),
			E('div', { 'style': 'font-weight:700;color:#0645c8;white-space:nowrap;text-align:right' }, ownedBenefitAmount(benefit))
		]),
		progressBar(pct(benefit.remaining, benefit.total), '13px')
	])));
}

function ownedPackageDetailBody(pkg, detail) {
	const option = detail.package_option || {};
	const family = detail.package_family || {};
	const variant = detail.package_detail_variant || {};
	const title = compactTitle([ family.name, variant.name, option.name ], option.package_option_code || pkg.name || _('Package'));
	const tnc = cleanPackageText(option.tnc || option.description || '');
	const familyCode = family.package_family_code || family.family_code || '';
	const optionCode = option.package_option_code || firstValue(pkg, [ 'quota_code', 'code' ], '');
	const infoRows = ownedPackageInfoRows(pkg, detail);
	const technicalRows = ownedPackageTechnicalRows(pkg);
	return E('div', {}, [
		E('div', { 'style': 'font-weight:700;font-size:1.18em;line-height:1.25;margin-bottom:.25em' }, title),
		packageCodeMeta(familyCode, optionCode),
		E('div', { 'style': 'border-top:1px solid ' + SOFT_LINE + ';margin:.75em 0 .8em' }),
		infoRows.length ? packageInfoGrid(infoRows) : '',
		E('div', { 'style': 'font-weight:700;margin:.2em 0 .45em' }, _('Quota Detail')),
		ownedBenefitRows(option.benefits),
		tnc ? E('details', { 'style': 'margin-top:1em' }, [
			E('summary', { 'style': 'cursor:pointer;font-weight:650' }, _('Terms')),
			E('div', { 'style': 'white-space:pre-wrap;margin-top:.55em;color:inherit;opacity:.72;line-height:1.45' }, tnc)
		]) : '',
		technicalRows.length ? E('details', { 'style': 'margin-top:1em' }, [
			E('summary', { 'style': 'cursor:pointer;font-weight:650' }, _('Technical')),
			E('div', { 'style': 'margin-top:.65em' }, packageInfoGrid(technicalRows))
		]) : '',
		E('div', { 'style': 'display:flex;justify-content:flex-end;margin-top:1.1em' }, [
			E('button', { 'class': 'btn cbi-button', 'click': () => ui.hideModal() }, _('Close'))
		])
	]);
}

function showOwnedPackageDetail(pkg) {
	const body = E('div', {}, _('Loading package detail...'));
	ui.showModal(_('Package Detail'), [ body ]);
	const code = firstValue(pkg, [ 'quota_code', 'code' ], '');
	const render = (remoteDetail) => replaceChildren(body, ownedPackageDetailBody(pkg, mergeOwnedPackageDetail(pkg, remoteDetail)));
	if (!code)
		return render(null);
	return callBitsxl([ 'json', 'package', 'detail', code ]).then((res) => {
		const detail = res && res.ok ? packageDetailData(res) : null;
		render(detail);
	}).catch(() => render(null));
}

function quotaPackageCards(quota, error) {
	if (error)
		return [ E('div', { 'class': 'alert-message warning' }, error) ];
	const packages = quota && Array.isArray(quota.quotas) ? quota.quotas : [];
	if (!packages.length)
		return [ E('div', { 'class': 'alert-message warning' }, _('No quota data.')) ];
	return packages.map((pkg, index) => E('div', {
		'role': 'button',
		'tabindex': '0',
		'style': 'margin-bottom:1em;padding:1em;border:1px solid transparent;border-radius:6px;background:' + SOFT_BORDER + ';cursor:pointer',
		'click': () => showOwnedPackageDetail(pkg),
		'keydown': (ev) => {
			if (ev.key === 'Enter' || ev.key === ' ') {
				ev.preventDefault();
				showOwnedPackageDetail(pkg);
			}
		}
	}, [
		E('div', { 'style': 'display:grid;grid-template-columns:2em minmax(0,1fr) 2.35em;gap:.75em;align-items:flex-start;margin-bottom:.7em' }, [
			E('div', { 'style': 'width:2em;height:2em;line-height:2em;text-align:center;border-radius:4px;background:rgba(127,127,127,.18);color:inherit;font-weight:600' }, String(index + 1)),
			E('div', { 'style': 'min-width:0;flex:1' }, [
				E('div', { 'style': 'font-size:1.05em;font-weight:600;line-height:1.25' }, pkg.name || '(no name)'),
				E('div', { 'style': 'color:inherit;opacity:.55;margin-top:.2em' }, pkg.group_name || '')
			]),
			packageDeleteButton(pkg)
		]),
		...((pkg.benefits || []).map(benefitCard))
	]));
}

function fmtMoney(value) {
	if (value == null || value === '')
		return 'IDR -';
	const number = Number(value);
	if (isNaN(number))
		return 'IDR ' + String(value);
	return 'IDR ' + String(Math.round(number)).replace(/\B(?=(\d{3})+(?!\d))/g, '.');
}

function firstValue(object, keys, fallback) {
	for (let index = 0; index < keys.length; index++) {
		const value = object && object[keys[index]];
		if (value != null && value !== '')
			return value;
	}
	return fallback;
}

function hasAny(text, words) {
	text = String(text || '').toUpperCase();
	for (let index = 0; index < words.length; index++)
		if (text.indexOf(words[index]) >= 0)
			return true;
	return false;
}

function isPrioritySubscription(type) {
	return hasAny(type, [ 'PRIO', 'PRIORITAS', 'HYBRID', 'POSTPAID' ]);
}

function isPriorityBanner(banner) {
	return hasAny([
		banner && banner.title,
		banner && banner.family_name,
		banner && banner.background_image_url,
		banner && banner.segment_image_url,
		banner && banner.action_param
	].join(' '), [ 'PRIO', 'PRIORITAS', 'HYBRID', 'POSTPAID' ]);
}

function segmentDiscountPercent(banner) {
	const explicit = Number(banner && banner.discount_percentage || 0);
	if (explicit > 0)
		return explicit;
	const original = Number(banner && banner.original_price || 0);
	const discounted = Number(banner && banner.discounted_price || 0);
	return original > 0 && discounted >= 0 && discounted < original ? Math.round((original - discounted) * 100 / original) : 0;
}

function cleanPackageText(value) {
	return String(value || '')
		.replace(/<br\s*\/?>/gi, '\n')
		.replace(/<\/p>/gi, '\n')
		.replace(/<[^>]*>/g, ' ')
		.replace(/&nbsp;/g, ' ')
		.replace(/&amp;/g, '&')
		.replace(/&lt;/g, '<')
		.replace(/&gt;/g, '>')
		.replace(/\s+\n/g, '\n')
		.replace(/\n\s+/g, '\n')
		.replace(/[ \t]{2,}/g, ' ')
		.trim();
}

function packageDetailData(res) {
	const detail = res && res.detail;
	if (!detail || typeof detail !== 'object')
		return null;
	const payload = detail.data || detail.response && detail.response.data || detail.response || detail;
	return payload && typeof payload === 'object' ? payload : null;
}

function packageDetailError(res, detail) {
	const raw = res && (res.detail || res.response || res);
	const status = raw && raw.status != null ? String(raw.status).toUpperCase() : '';
	if (status && status !== 'SUCCESS')
		return firstValue(raw, [ 'message', 'error', 'description', 'code_detail', 'code' ], _('Package detail failed.'));
	if (!detail || !detail.package_option)
		return _('Package detail response missing package_option.');
	return '';
}

function rawDetailError(message, raw) {
	return E('div', {}, [
		E('div', { 'class': 'alert-message warning' }, message),
		E('details', { 'open': true, 'style': 'margin-top:.75em' }, [
			E('summary', { 'style': 'cursor:pointer;font-weight:650' }, _('Raw response')),
			E('pre', { 'style': 'margin-top:.6em;max-height:24em;overflow:auto;white-space:pre-wrap;word-break:break-word;background:rgba(127,127,127,.08);border:1px solid ' + SOFT_LINE + ';border-radius:6px;padding:.75em' }, logText(raw))
		])
	]);
}

function packageCodeMeta(family, option, options) {
	options = options || {};
	const rows = [
		{ label: _('Family'), value: family },
		{ label: _('Option'), value: option }
	].filter((row) => row.value);
	if (!rows.length)
		return '';
	return E('div', { 'style': 'display:flex;gap:.35em;flex-wrap:wrap;margin:.45em 0 1em' }, rows.map((row) => E('span', { 'style': 'display:inline-flex;align-items:center;gap:.3em;min-width:0;max-width:100%;border:1px solid ' + SOFT_LINE + ';border-radius:4px;padding:.16em .35em;color:inherit;opacity:.72;background:rgba(127,127,127,.05);font-size:.86em' }, [
		E('span', { 'style': 'white-space:nowrap;font-weight:650' }, row.label),
		E('code', { 'style': 'min-width:0;max-width:12em;overflow:hidden;text-overflow:ellipsis;white-space:nowrap' }, row.value),
		options.copy === false ? '' : E('button', { 'class': 'btn cbi-button', 'style': 'padding:.05em .35em;font-size:.9em', 'click': (ev) => copyText(row.value, ev) }, _('Copy'))
	])));
}

function compactTitle(parts, fallback) {
	const seen = {};
	const out = [];
	(parts || []).forEach((part) => {
		part = String(part || '').trim();
		if (part && !seen[part]) {
			seen[part] = true;
			out.push(part);
		}
	});
	return out.length ? out.join(' - ') : fallback;
}

function packageBenefitAmount(benefit) {
	if (benefit && benefit.is_unlimited)
		return _('Unlimited');
	return fmtQuota(benefit && benefit.total, benefit && benefit.data_type);
}

function packageBenefitCards(benefits) {
	if (!Array.isArray(benefits) || !benefits.length)
		return E('div', { 'class': 'alert-message warning', 'style': 'margin-top:.75em' }, _('No quota detail.'));
	return E('div', { 'style': 'margin-top:.45em;border-top:1px solid ' + SOFT_LINE }, benefits.map((benefit) => E('div', { 'style': 'display:grid;grid-template-columns:minmax(0,1fr) auto;gap:.5em 1em;align-items:center;padding:.75em 0;border-bottom:1px solid ' + SOFT_LINE }, [
		E('div', { 'style': 'min-width:0' }, [
			E('div', { 'style': 'font-weight:650;line-height:1.25' }, benefit.name || _('Benefit')),
			E('div', { 'style': 'color:inherit;opacity:.55;font-size:.9em;margin-top:.2em' }, benefit.data_type || 'DATA')
		]),
		E('div', { 'style': 'font-weight:700;color:#0645c8;white-space:nowrap;text-align:right' }, packageBenefitAmount(benefit))
	])));
}

function normalizeMsisdn(value) {
	value = String(value || '').trim().replace(/[^0-9]/g, '');
	if (value.indexOf('08') === 0)
		return '62' + value.slice(1);
	if (value.indexOf('8') === 0)
		return '62' + value;
	return value;
}

function normalizeWalletNumber(value) {
	value = String(value || '').trim().replace(/[^0-9]/g, '');
	if (value.indexOf('628') === 0)
		return '0' + value.slice(2);
	if (value.indexOf('8') === 0)
		return '0' + value;
	return value;
}

function paymentMode(value) {
	for (let index = 0; index < PAYMENT_MODES.length; index++)
		if (PAYMENT_MODES[index].value === value)
			return PAYMENT_MODES[index];
	return PAYMENT_MODES[0];
}

function paymentArgs(mode, items, walletNumber, destinationMsisdn, customAmount) {
	const selected = paymentMode(mode);
	const args = [ 'json', 'payment', selected.command ].concat(items.map((item) => item.code));
	if (selected.wallet)
		args.push(walletNumber);
	if (selected.destination)
		args.push(destinationMsisdn);
	if (customAmount && !selected.single)
		args.push('amount=' + customAmount);
	args.push('confirm=1');
	return args;
}

function customAmountValue(value) {
	value = String(value || '').trim();
	if (!value)
		return '';
	value = value.replace(/rp/ig, '').replace(/[.,\s]/g, '');
	if (!/^[0-9]+$/.test(value))
		return null;
	const number = Number(value);
	if (!Number.isSafeInteger(number) || number <= 0)
		return null;
	return value;
}

function paymentField(res, key) {
	if (!res)
		return '';
	if (res[key])
		return res[key];
	if (res.response && res.response[key])
		return res.response[key];
	if (res.response && res.response.data && res.response.data[key])
		return res.response.data[key];
	if (res.pending_detail && res.pending_detail[key])
		return res.pending_detail[key];
	if (res.pending_detail && res.pending_detail.data && res.pending_detail.data[key])
		return res.pending_detail.data[key];
	return '';
}

function paymentFollowupPanel(res, okText) {
	const qrisUrl = paymentField(res, 'qris_url');
	const qrisCode = paymentField(res, 'qris_code');
	const deeplink = paymentField(res, 'deeplink');
	const isQris = res && res.payment === 'qris' || qrisUrl || qrisCode;
	const isEwallet = res && res.payment === 'ewallet' || deeplink;
	if (!isQris && !isEwallet)
		return null;
	const link = isQris ? qrisUrl : deeplink;
	const raw = isQris ? qrisCode : deeplink;
	return E('div', {}, [
		E('div', { 'class': 'alert-message' }, okText || (isQris ? _('QRIS payment created.') : _('E-wallet payment created.'))),
		link ? E('div', { 'style': 'margin-top:.85em' }, [
			E('a', { 'class': 'btn cbi-button cbi-button-save', 'href': link, 'target': '_blank', 'rel': 'noopener' }, isQris ? _('Open QRIS') : _('Open payment'))
		]) : '',
		res.transaction_code ? E('div', { 'style': 'margin-top:.75em;color:inherit;opacity:.7;word-break:break-word' }, [ _('Transaction'), ': ', res.transaction_code ]) : '',
		raw ? E('pre', { 'style': 'margin-top:.75em;max-height:16em;overflow:auto;white-space:pre-wrap;word-break:break-word;background:rgba(127,127,127,.08);border:1px solid ' + SOFT_LINE + ';border-radius:6px;padding:.75em' }, raw) : '',
		E('details', { 'style': 'margin-top:.75em' }, [
			E('summary', { 'style': 'cursor:pointer;font-weight:650' }, _('Server response')),
			E('pre', { 'style': 'margin-top:.6em;max-height:20em;overflow:auto;white-space:pre-wrap;word-break:break-word;background:rgba(127,127,127,.08);border:1px solid ' + SOFT_LINE + ';border-radius:6px;padding:.75em' }, logText(res))
		]),
		E('div', { 'style': 'display:flex;justify-content:flex-end;margin-top:1em' }, [
			E('button', { 'class': 'btn cbi-button', 'click': () => ui.hideModal() }, _('Close'))
		])
	]);
}

function paymentResultPanel(res, okText) {
	const status = logField(res, [ 'status', 'payment_status', 'code' ]) || (res && res.ok ? 'SUCCESS' : 'UNKNOWN');
	const message = logField(res, [ 'message', 'error', 'description', 'title', 'code_detail' ]) || okText || _('Payment response received.');
	return E('div', {}, [
		E('div', { 'class': res && res.ok ? 'alert-message' : 'alert-message warning' }, [
			E('div', {}, [ E('strong', {}, _('Status')), ': ', String(status) ]),
			message ? E('div', { 'style': 'margin-top:.3em;overflow-wrap:anywhere' }, [ E('strong', {}, _('Message')), ': ', String(message) ]) : ''
		]),
		E('details', { 'open': true, 'style': 'margin-top:.75em' }, [
			E('summary', { 'style': 'cursor:pointer;font-weight:650' }, _('Server response')),
			E('pre', { 'style': 'margin-top:.6em;max-height:24em;overflow:auto;white-space:pre-wrap;word-break:break-word;background:rgba(127,127,127,.08);border:1px solid ' + SOFT_LINE + ';border-radius:6px;padding:.75em' }, logText(res))
		]),
		E('div', { 'style': 'display:flex;justify-content:flex-end;gap:.6em;margin-top:1em;flex-wrap:wrap' }, [
			E('button', { 'class': 'btn cbi-button', 'click': () => ui.hideModal() }, _('Close')),
			E('button', { 'class': 'btn cbi-button cbi-button-reload', 'click': () => window.location.reload() }, _('Refresh'))
		])
	]);
}

function checkoutPaymentItems(items, options) {
	items = (items || []).filter((item) => item && item.code);
	options = options || {};
	if (!items.length)
		return notifyResult({ ok: false, error: _('No package selected.') });
	const total = items.reduce((sum, item) => sum + Number(item.price || 0), 0);
	const modeSelect = E('select', { 'class': 'cbi-input-select' }, PAYMENT_MODES.map((mode) => E('option', { 'value': mode.value }, mode.label)));
	const wallet = E('input', { 'class': 'cbi-input-text', 'type': 'text', 'placeholder': '08xxxx / 628xxxx', 'style': 'max-width:14em' });
	const destination = E('input', { 'class': 'cbi-input-text', 'type': 'text', 'placeholder': '08xxxx / 628xxxx', 'style': 'max-width:14em' });
	const customAmount = E('input', { 'class': 'cbi-input-text', 'type': 'text', 'inputmode': 'numeric', 'placeholder': String(total || 0), 'style': 'max-width:14em' });
	const walletRow = E('div', { 'style': 'display:none;margin-top:.6em' }, [ E('label', { 'style': 'display:block;font-weight:650;margin-bottom:.25em' }, _('Wallet number')), wallet ]);
	const destinationRow = E('div', { 'style': 'display:none;margin-top:.6em' }, [ E('label', { 'style': 'display:block;font-weight:650;margin-bottom:.25em' }, _('Destination MSISDN')), destination ]);
	const customAmountRow = E('div', { 'style': 'display:none;margin-top:.6em' }, [
		E('label', { 'style': 'display:block;font-weight:650;margin-bottom:.25em' }, _('Custom price')),
		customAmount,
		E('div', { 'style': 'margin-top:.25em;color:inherit;opacity:.62' }, _('Blank uses quoted total.'))
	]);
	const modeHint = E('div', { 'style': 'display:none;margin-top:.55em;color:inherit;opacity:.66' });
	const refreshModeInputs = () => {
		const selected = paymentMode(modeSelect.value);
		walletRow.style.display = selected.wallet ? 'block' : 'none';
		destinationRow.style.display = selected.destination ? 'block' : 'none';
		customAmountRow.style.display = selected.single ? 'none' : 'block';
		if (selected.value === 'qris') {
			modeHint.style.display = 'block';
			replaceChildren(modeHint, E('span', {}, _('QRIS link appears after checkout.')));
		} else if (selected.ewallet) {
			modeHint.style.display = 'block';
			replaceChildren(modeHint, E('span', {}, _('E-wallet payment link appears after checkout.')));
		} else {
			modeHint.style.display = 'none';
			replaceChildren(modeHint, '');
		}
	};
	modeSelect.addEventListener('change', refreshModeInputs);
	const buy = E('button', {
		'class': 'btn cbi-button cbi-button-save',
		'click': () => {
			const selected = paymentMode(modeSelect.value);
			const walletNumber = normalizeWalletNumber(wallet.value);
			const destinationMsisdn = normalizeMsisdn(destination.value);
			wallet.value = walletNumber;
			destination.value = destinationMsisdn;
			if (selected.single && items.length !== 1)
				return notifyResult({ ok: false, error: _('This payment mode supports one package only.') });
			if (selected.wallet && !walletNumber)
				return notifyResult({ ok: false, error: _('Wallet number required.') });
			if (selected.wallet && !/^08[0-9]{8,11}$/.test(walletNumber))
				return notifyResult({ ok: false, error: _('Wallet number must start with 08 and contain 10-13 digits.') });
			if (selected.destination && !destinationMsisdn)
				return notifyResult({ ok: false, error: _('Destination MSISDN required.') });
			if (selected.destination && !/^628[0-9]{5,11}$/.test(destinationMsisdn))
				return notifyResult({ ok: false, error: _('Destination MSISDN must start with 628.') });
			const customTotal = selected.single ? '' : customAmountValue(customAmount.value);
			if (customTotal == null)
				return notifyResult({ ok: false, error: _('Custom price must be a positive number.') });
			buy.disabled = true;
			let logged = false;
			return callBitsxl(paymentArgs(selected.value, items, walletNumber, destinationMsisdn, customTotal)).then((res) => {
				savePaymentLog('dashboard', selected.value, items, res, customTotal);
				logged = true;
				if (!res || !res.ok)
					throw paymentError((res && (res.error || res.message)) || _('Checkout failed.'), res);
				const followup = paymentFollowupPanel(res, options.okText);
				if (followup) {
					replaceChildren(body, followup);
					return;
				}
				replaceChildren(body, paymentResultPanel(res, options.okText || _('Payment sent.')));
			}).catch((err) => {
				if (!logged)
					savePaymentLog('dashboard', selected.value, items, err && err.payload != null ? err.payload : { ok: false, error: err && err.message || String(err) }, customTotal);
				buy.disabled = false;
				showPaymentError(err);
			});
		}
	}, _('Checkout'));
	const body = E('div', {}, [
		E('div', { 'style': 'border-top:1px solid ' + SOFT_LINE }, items.map((item) => E('div', { 'style': 'display:grid;grid-template-columns:minmax(0,1fr) auto;gap:1em;padding:.6em 0;border-bottom:1px solid ' + SOFT_LINE }, [
			E('div', { 'style': 'font-weight:650' }, item.name || item.code),
			E('div', { 'style': 'font-weight:700;color:#0645c8;white-space:nowrap' }, fmtMoney(item.price))
		]))),
		E('div', { 'style': 'display:flex;justify-content:space-between;gap:1em;margin-top:.8em;font-weight:700' }, [ _('Total'), fmtMoney(total) ]),
		E('div', { 'style': 'margin-top:1em' }, [ E('label', { 'style': 'display:block;font-weight:650;margin-bottom:.25em' }, _('Payment')), modeSelect ]),
		modeHint,
		customAmountRow,
		walletRow,
		destinationRow,
		E('div', { 'style': 'display:flex;gap:.6em;justify-content:flex-end;margin-top:1em;flex-wrap:wrap' }, [
			E('button', { 'class': 'btn cbi-button', 'click': () => ui.hideModal() }, _('Cancel')),
			buy
		])
	]);
	refreshModeInputs();
	ui.showModal(options.title || _('Checkout'), [ body ]);
}

function showPackageDetail(actionType, actionParam, sourceItem) {
	if (actionType !== 'PDP' || !actionParam) {
		ui.addNotification(null, E('p', {}, _('%s: %s').format(actionType || 'Action', actionParam || '-')), 'info');
		return;
	}
	const body = E('div', {}, _('Loading package detail...'));
	ui.showModal(_('Package Detail'), [ body ]);
	const familyCode = firstValue(sourceItem || {}, [ 'package_family_code', 'family_code' ], '');
	const variantCode = firstValue(sourceItem || {}, [ 'package_variant_code', 'variant_code' ], '');
	const args = [ 'json', 'package', 'detail', actionParam ];
	if (familyCode || variantCode)
		args.push(familyCode || '-', variantCode || '-', isPriorityBanner(sourceItem || {}) ? '1' : '0');
	callBitsxl(args).then((res) => {
		if (!res || !res.ok) {
			replaceChildren(body, rawDetailError((res && res.error) || _('Failed to load package detail.'), res && (res.response || res.detail || res)));
			return;
		}
		const detail = packageDetailData(res);
		const detailError = packageDetailError(res, detail);
		if (detailError) {
			replaceChildren(body, rawDetailError(detailError, res && (res.detail || res.response || res)));
			return;
		}
		const option = detail.package_option || {};
		const family = detail.package_family || {};
		const variant = detail.package_detail_variant || {};
		const title = compactTitle([ family.name, variant.name, option.name ], actionParam);
		const item = { code: actionParam, name: title, price: option.price, validity: option.validity || '-' };
		replaceChildren(body, packageDetailBody(detail, actionParam, {
			footer: E('div', { 'style': 'display:flex;gap:.6em;justify-content:flex-end;margin-top:1.1em;flex-wrap:wrap' }, [
				E('button', { 'class': 'btn cbi-button', 'click': () => ui.hideModal() }, _('Close')),
				E('button', { 'class': 'btn cbi-button cbi-button-save', 'style': BUY_STYLE, 'click': () => checkoutPaymentItems([ item ], { title: _('Buy Package'), okText: _('Purchase complete.') }) }, _('Buy'))
			])
		}));
	});
}

function showPaymentQuote(actionType, actionParam, item) {
	if (actionType !== 'PDP' || !actionParam) {
		ui.addNotification(null, E('p', {}, _('%s: %s').format(actionType || 'Action', actionParam || '-')), 'info');
		return;
	}
	return checkoutPaymentItems([ item || { code: actionParam, name: actionParam, price: 0, validity: '-' } ], { title: _('Buy Package'), okText: _('Purchase complete.') });
}

function segmentCards(segments, subscriptionType) {
	const rows = [];
	const priorityStore = isPrioritySubscription(subscriptionType);
	segments.forEach((segment) => {
		const banners = segment.banners || [];
		if (!banners.length)
			return;
		rows.push(E('div', { 'style': 'margin-bottom:1.1em' }, [
			E('h3', { 'style': 'margin:.2em 0 .6em' }, segment.title || _('Store Segments')),
			E('div', { 'style': 'display:flex;gap:1em;overflow-x:auto;overflow-y:hidden;padding:.25em .1em 1em;scroll-snap-type:x mandatory;-webkit-overflow-scrolling:touch' }, banners.map((banner) => {
				const title = banner.title || 'N/A';
				const family = banner.family_name || banner.upper_title || '';
				const validity = banner.validity || '';
				const actionType = banner.action_type || '';
				const actionParam = banner.action_param || '';
				const price = firstValue(banner, [ 'discounted_price', 'price', 'original_price' ], '');
				const priority = priorityStore || isPriorityBanner(banner);
				const original = Number(banner.original_price || 0);
				const discount = segmentDiscountPercent(banner);
				return E('div', {
					'role': 'button',
					'tabindex': '0',
					'class': 'cbi-section',
					'style': 'flex:0 0 clamp(15em,42vw,18em);scroll-snap-align:start;margin:0;padding:.9em 1em;border:1px solid transparent;border-radius:6px;background:' + SOFT_BORDER + ';cursor:pointer',
					'click': () => showPackageDetail(actionType, actionParam, banner),
					'keydown': (ev) => { if (ev.key === 'Enter' || ev.key === ' ') showPackageDetail(actionType, actionParam, banner); }
				}, [
					priority ? E('div', { 'style': 'display:flex;gap:.4em;flex-wrap:wrap;margin-bottom:.55em' }, [
						E('span', { 'style': 'border:1px solid ' + SOFT_LINE + ';border-radius:4px;padding:.18em .45em;color:inherit;opacity:.72;font-size:.88em;font-weight:650;background:rgba(127,127,127,.06)' }, _('PRIO'))
					]) : '',
					E('div', { 'style': 'display:grid;grid-template-columns:minmax(0,1fr) auto;gap:.75em;align-items:start' }, [
						E('div', { 'style': 'min-width:0' }, [
							E('div', { 'style': 'font-size:1.08em;font-weight:650;line-height:1.25' }, title),
							family ? E('div', { 'style': 'margin-top:.3em;color:inherit;opacity:.6;white-space:nowrap;overflow:hidden;text-overflow:ellipsis' }, family) : ''
						]),
						E('div', { 'style': 'text-align:right;white-space:nowrap' }, [
							E('div', { 'style': 'font-weight:650;color:inherit;opacity:.72' }, validity || '-'),
							E('div', { 'style': 'margin-top:.45em;color:#0645c8;font-weight:700' }, fmtMoney(price))
						])
					]),
					discount && original > Number(price) ? E('div', { 'style': 'margin-top:.45em;display:flex;align-items:center;gap:.4em;flex-wrap:wrap;color:inherit' }, [
						E('span', { 'style': 'opacity:.52;text-decoration:line-through;font-weight:650' }, fmtMoney(original)),
						E('span', { 'style': 'border:1px solid rgba(6,69,200,.20);border-radius:4px;padding:.1em .35em;color:#0645c8;background:rgba(6,69,200,.06);font-size:.82em;font-weight:700;line-height:1.4' }, '-%d%%'.format(discount))
					]) : ''
				]);
			}))
		]));
	});
	return rows.length ? E('div', { 'class': 'cbi-section', 'style': 'margin-top:1em' }, rows) : '';
}

function replaceChildren(node, child) {
	while (node.firstChild)
		node.removeChild(node.firstChild);
	if (child)
		node.appendChild(child);
}

function storeSegmentsPanel(subscriptionType) {
	const box = E('div', {}, E('div', { 'class': 'cbi-section', 'style': 'margin-top:1em;color:inherit;opacity:.6' }, _('Loading Store Segments...')));
	window.setTimeout(() => {
		callBitsxl([ 'json', 'segments' ]).then((res) => {
			if (!res || !res.ok) {
				replaceChildren(box, '');
				return;
			}
			replaceChildren(box, segmentCards(storeSegments(res), subscriptionType));
		});
	}, 0);
	return box;
}

function activeText(epoch) {
	epoch = Number(epoch || 0);
	if (!epoch)
		return 'N/A';
	const days = Math.ceil((epoch * 1000 - Date.now()) / 86400000);
	if (days < 0)
		return _('Expired') + ' · ' + fmtDate(epoch);
	return '%d hari · %s'.format(days, fmtDate(epoch));
}

function quotaSummary(quota) {
	let remaining = 0, total = 0;
	((quota && quota.quotas) || []).forEach((pkg) => {
		(pkg.benefits || []).forEach((benefit) => {
			if (String(benefit.data_type || '').toUpperCase() !== 'DATA')
				return;
			remaining += Number(benefit.remaining || 0);
			total += Number(benefit.total || 0);
		});
	});
	return { remaining: remaining, total: total, percent: pct(remaining, total) };
}

function summaryBar(percent) {
	percent = Math.max(0, Math.min(100, Number(percent || 0)));
	const width = percent ? Math.max(3, percent) : 0;
	return E('div', { 'style': 'height:16px;border-radius:999px;background:' + SOFT_TRACK + ';overflow:hidden' },
		E('div', { 'style': 'height:100%;width:' + width + '%;border-radius:999px;background:linear-gradient(90deg,#0ea5e9,#16c784)' }));
}

function statLabel(text) {
	return E('div', { 'style': 'font-size:.9em;color:inherit;opacity:.55;margin-bottom:.35em' }, text);
}

function statCard(children, extraStyle) {
	return E('div', { 'style': 'flex:1 1 0;min-width:0;box-sizing:border-box;padding:.85em;border:1px solid transparent;border-radius:6px;background:' + SOFT_BORDER + ';%s'.format(extraStyle || '') }, children);
}

function balanceStatCard(balance) {
	const credit = creditData(balance);
	const nested = credit && credit.balance && typeof credit.balance === 'object' ? credit.balance : {};
	const children = [
		statLabel(_('Balance')),
		E('div', { 'style': 'font-size:clamp(1.2em,4vw,1.55em);font-weight:600;margin-bottom:.65em;white-space:nowrap' }, balanceValue(balance)),
		statLabel(_('Masa aktif')),
		E('div', { 'style': 'font-size:clamp(.9em,3.2vw,1.1em);line-height:1.25' }, activeText(balanceExpiry(balance)))
	];
	if (credit) {
		children.push(E('div', { 'style': 'margin:.75em 0 .65em;border-top:1px solid ' + SOFT_LINE }));
		children.push(statLabel(_('Credit/Pulsa')));
		children.push(E('div', { 'style': 'font-size:clamp(1.05em,3.5vw,1.3em);font-weight:600;white-space:nowrap' }, fmtRupiah(firstValue(credit, [ 'remaining', 'amount', 'value', 'raw_balance' ], firstValue(nested, [ 'remaining', 'amount', 'value', 'raw_balance' ], null)))));
	}
	return statCard(children);
}

function miniStat(label, value) {
	return E('div', { 'style': 'min-width:0;box-sizing:border-box;padding:.55em .7em;border:1px solid transparent;border-radius:6px;background:' + SOFT_BORDER + ';display:flex;align-items:center;justify-content:space-between;gap:.75em' }, [
		E('div', { 'style': 'font-size:.86em;color:inherit;opacity:.58;white-space:nowrap' }, label),
		E('div', { 'style': 'font-size:1em;font-weight:650;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;text-align:right' }, value)
	]);
}

function tieringMiniCards(tiering) {
	if (!tiering)
		return '';
	return E('div', { 'style': 'display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:.65em;margin-top:.65em' }, [
		miniStat(_('XL Poin'), tiering.point === '' ? 'N/A' : String(tiering.point)),
		miniStat(_('Tier'), tiering.tier === '' ? 'N/A' : String(tiering.tier))
	]);
}

function tieringMiniFallback(text) {
	return E('div', { 'style': 'display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:.65em;margin-top:.65em' }, [
		miniStat(_('XL Poin'), text),
		miniStat(_('Tier'), text)
	]);
}

function loadTiering(box) {
	if (tieringLoading || tieringLoaded)
		return;
	tieringLoading = true;
	tieringError = '';
	replaceChildren(box, tieringMiniFallback(_('Loading...')));
	callBitsxl([ 'json', 'tiering' ]).then((res) => {
		tieringLoading = false;
		tieringLoaded = true;
		tieringResult = res;
		tieringError = res && res.ok !== false ? '' : (res && (res.error || res.message)) || _('Failed to load tiering.');
		replaceChildren(box, tieringError ? tieringMiniFallback('N/A') : (tieringMiniCards(tieringData(res)) || tieringMiniFallback('N/A')));
	}, (err) => {
		tieringLoading = false;
		tieringError = err && err.message || String(err);
		replaceChildren(box, tieringMiniFallback('N/A'));
	});
}

function tieringMiniPanel() {
	const box = E('div');
	if (tieringLoaded)
		replaceChildren(box, tieringError ? tieringMiniFallback('N/A') : (tieringMiniCards(tieringData(tieringResult)) || tieringMiniFallback('N/A')));
	else
		window.setTimeout(() => loadTiering(box), 0);
	return box;
}

function transactionTitle(tx) {
	return firstValue(tx, [ 'title', 'package_name', 'payment_for', 'category', 'code' ], _('Transaction'));
}

function transactionDate(tx) {
	const text = firstValue(tx, [ 'formated_date', 'formatted_date', 'date' ], '');
	if (text)
		return text;
	const timestamp = firstValue(tx, [ 'timestamp', 'created_at' ], 0);
	return timestamp ? fmtDate(timestamp) : '';
}

function transactionPrice(tx) {
	const text = firstValue(tx, [ 'price', 'amount' ], '');
	if (text)
		return String(text);
	return fmtMoney(firstValue(tx, [ 'raw_price', 'total_amount' ], null));
}

function transactionRow(tx) {
	const method = firstValue(tx, [ 'payment_method_label', 'payment_with_label', 'payment_method', 'payment_with' ], '');
	const status = firstValue(tx, [ 'payment_status', 'status' ], '-');
	const meta = [ transactionDate(tx), method ].filter(Boolean).join(' · ');
	return E('div', { 'style': 'display:grid;grid-template-columns:minmax(0,1fr) auto;gap:.75em 1em;padding:.75em 0;border-top:1px solid ' + SOFT_LINE }, [
		E('div', { 'style': 'min-width:0' }, [
			E('div', { 'style': 'font-weight:650;line-height:1.25' }, transactionTitle(tx)),
			meta ? E('div', { 'style': 'margin-top:.25em;color:inherit;opacity:.58;font-size:.92em;line-height:1.25' }, meta) : ''
		]),
		E('div', { 'style': 'text-align:right;white-space:nowrap' }, [
			E('div', { 'style': 'font-weight:700;color:#0645c8' }, transactionPrice(tx)),
			E('div', { 'style': 'margin-top:.25em;color:inherit;opacity:.68;font-size:.92em' }, status)
		])
	]);
}

function transactionHistoryBody() {
	if (transactionHistoryLoading)
		return E('div', { 'class': 'alert-message' }, _('Loading transaction history...'));
	if (transactionHistoryError)
		return E('div', { 'class': 'alert-message warning' }, transactionHistoryError);
	if (!transactionHistoryLoaded)
		return '';
	const transactions = transactionHistoryData(transactionHistoryResult);
	return E('div', {}, [
		...(transactions.length ? transactions.slice(0, 3).map(transactionRow) : [ E('div', { 'class': 'alert-message warning' }, _('No transaction history.')) ])
	]);
}

function loadTransactionHistory(body) {
	if (transactionHistoryLoading || transactionHistoryLoaded)
		return;
	transactionHistoryLoading = true;
	transactionHistoryError = '';
	replaceChildren(body, transactionHistoryBody());
	callBitsxl([ 'json', 'transaction-history' ]).then((res) => {
		transactionHistoryLoading = false;
		transactionHistoryLoaded = true;
		transactionHistoryResult = res;
		transactionHistoryError = res && res.ok !== false ? '' : (res && (res.error || res.message)) || _('Failed to load transaction history.');
		replaceChildren(body, transactionHistoryBody());
	}, (err) => {
		transactionHistoryLoading = false;
		transactionHistoryError = err && err.message || String(err);
		replaceChildren(body, transactionHistoryBody());
	});
}

function transactionSummaryPanel() {
	const body = E('div', { 'style': transactionHistoryExpanded ? 'display:block' : 'display:none' });
	const toggle = E('button', {
		'class': 'btn cbi-button',
		'title': _('Expand transaction history'),
		'click': () => {
			transactionHistoryExpanded = !transactionHistoryExpanded;
			body.style.display = transactionHistoryExpanded ? 'block' : 'none';
			toggle.textContent = transactionHistoryExpanded ? '▼' : '▲';
			if (transactionHistoryExpanded)
				loadTransactionHistory(body);
		}
	}, transactionHistoryExpanded ? '▼' : '▲');
	if (transactionHistoryExpanded)
		window.setTimeout(() => loadTransactionHistory(body), 0);
	return E('div', { 'class': 'cbi-section', 'style': 'margin-top:1em' }, [
		E('div', { 'style': 'display:flex;align-items:center;justify-content:space-between;gap:1em' }, [
			E('h3', { 'style': 'margin:0' }, _('Transaction History')),
			toggle
		]),
		body
	]);
}

function openAccountModal(accountsData, currentNumber) {
	if (accountsData && accountsData.lazy && !accountsData.loaded) {
		ui.showModal(_('Switch Account'), [ E('div', { 'class': 'alert-message' }, _('Loading accounts...')) ]);
		return callBitsxl([ 'json', 'accounts' ]).then((res) => {
			accountsData.lazy = false;
			accountsData.loaded = true;
			accountsData.accounts = res && res.accounts || [];
			openAccountModal(accountsData, res && res.active || currentNumber);
		}, (err) => {
			ui.showModal(_('Switch Account'), [
				E('div', { 'class': 'alert-message warning' }, err && err.message || String(err)),
				E('button', { 'class': 'btn cbi-button', 'style': 'display:block;width:100%;margin-top:.75em', 'click': () => ui.hideModal() }, _('Cancel'))
			]);
		});
	}
	const accounts = accountsData.accounts || [];
	const rows = accounts.map((account) => E('div', { 'style': 'display:flex;gap:.5em;align-items:center;margin:.25em 0' }, [
		E('button', {
			'class': 'btn cbi-button',
			'style': 'flex:1;text-align:left;%s'.format(account.number === currentNumber ? 'background:rgba(127,127,127,.18);color:inherit' : ''),
			'click': () => {
				if (account.number === currentNumber) {
					ui.hideModal();
					return;
				}
				return callBitsxl([ 'json', 'use', account.number ]).then((res) => {
					notifyResult(res, _('Active account changed.'));
					ui.hideModal();
					if (res && res.ok)
						window.location.reload();
				});
			}
		}, '%s [%s]%s'.format(account.number, account.subscription_type || '-', account.number === currentNumber ? ' ✓' : '')),
		E('button', {
			'class': 'btn cbi-button cbi-button-remove',
			'click': () => {
				if (!confirm(_('Delete account %s?').format(account.number)))
					return;
				return callBitsxl([ 'json', 'del', account.number ]).then((res) => {
					notifyResult(res, _('Account deleted.'));
					ui.hideModal();
					if (res && res.ok)
						window.location.reload();
				});
			}
		}, _('Delete'))
	]));
	const body = rows.length ? rows : [];
	if (rows.length)
		body.push(E('div', { 'style': 'margin:.9em 0;border-top:1px solid ' + SOFT_LINE }));
	body.push(loginPanel());
	ui.showModal(_('Switch Account'), body);
}

function accountSwitchButton(accountsData, currentNumber) {
	const accounts = accountsData.accounts || [];
	const current = accounts.filter((account) => account.number === currentNumber)[0] || accounts.filter((account) => account.active)[0] || {};
	const label = current.number || currentNumber || _('Login');
	const type = current.subscription_type ? ' [%s]'.format(current.subscription_type) : '';
	return E('button', {
		'class': 'btn cbi-button',
		'style': 'text-align:left;min-width:16em',
		'click': () => {
			openAccountModal(accountsData, currentNumber);
		}
	}, '%s%s  ▼'.format(label, type));
}


function normalizeNumber(value) {
	value = String(value || '').trim().replace(/[^0-9]/g, '');
	if (value.indexOf('08') === 0)
		return '62' + value.slice(1);
	if (value.indexOf('8') === 0)
		return '62' + value;
	return value;
}

function quotaResponseData(response) {
	let payload = response && response.quota ? response.quota : response;
	if (payload && payload.response)
		payload = payload.response;
	if (payload && payload.data)
		payload = payload.data;
	return payload && typeof payload === 'object' ? payload : {};
}

function compactQuotaResponse(response) {
	const payload = quotaResponseData(response);
	const packages = Array.isArray(payload.quotas) ? payload.quotas : [];
	return {
		quotas: packages.map((pkg) => ({
			name: pkg.name || '',
			group_name: pkg.group_name || '',
			group_code: pkg.group_code || '',
			quota_code: pkg.quota_code || pkg.code || '',
			product_domain: pkg.product_domain || pkg.domain || '',
			product_subscription_type: pkg.product_subscription_type || pkg.subtype || '',
			parent_code: pkg.parent_code || '',
			is_addon: pkg.is_addon === true,
			benefits: (Array.isArray(pkg.benefits) ? pkg.benefits : []).map((benefit) => ({
				id: benefit.id || '',
				name: benefit.name || '',
				information: benefit.information || '',
				data_type: benefit.data_type || '',
				total: Number(benefit.total || 0),
				remaining: Number(benefit.remaining || 0),
				is_unlimited: benefit.is_unlimited === true,
				benefit_type: benefit.benefit_type || '',
				benefit_category: benefit.benefit_category || '',
				is_fup: benefit.is_fup === true
			}))
		}))
	};
}

function quotaResponseOk(response) {
	const payload = quotaResponseData(response);
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

function quotaHistoryUrl() {
	return L.url('admin/services/bitsxl/riwayat/quota-history');
}

function finishQuotaCheck(snapshot, button) {
	button.disabled = false;
	button.textContent = _('Check All Number Kuota');
	if (!saveQuotaSnapshot(snapshot)) {
		notifyResult({ ok: false, error: _('Quota result is too large to save in this browser.') });
		return;
	}
	ui.hideModal();
	const succeeded = Number(snapshot.succeeded || 0);
	const failed = Number(snapshot.failed || 0);
	ui.addNotification(null, E('p', {}, _('%d number checked, %d failed.').format(succeeded, failed)), failed ? 'warning' : 'info');
	window.location.href = quotaHistoryUrl();
}

function runQuotaCheckAll(button) {
	return callBitsxl([ 'json', 'accounts' ]).then((accountsResult) => {
		const accounts = accountsResult && Array.isArray(accountsResult.accounts) ? accountsResult.accounts : [];
		if (!accounts.length) {
			button.disabled = false;
			button.textContent = _('Check All Number Kuota');
			return notifyResult({ ok: false, error: _('No registered number found.') });
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
			snapshot.ok = snapshot.succeeded > 0;
			snapshot.complete = snapshot.failed === 0;
			finishQuotaCheck(snapshot, button);
		});
	});
}

function checkAllNumberQuota(button) {
	button.disabled = true;
	button.textContent = _('Checking all numbers...');
	return runQuotaCheckAll(button);
}

function loginPanel() {
	const number = E('input', { 'class': 'cbi-input-text', 'type': 'text', 'inputmode': 'numeric', 'placeholder': '08xxxx / 628xxxx', 'style': 'box-sizing:border-box;min-width:0;width:100%;max-width:10em' });
	const otp = E('input', { 'class': 'cbi-input-text', 'type': 'text', 'placeholder': 'OTP', 'style': 'max-width:7em' });
	const otpBox = E('div', { 'style': 'display:none;margin-top:.45em;text-align:left' }, [
		otp,
		' ',
		E('button', {
			'class': 'btn cbi-button cbi-button-save',
			'click': () => {
				const msisdn = normalizeNumber(number.value);
				number.value = msisdn;
				const code = otp.value.trim();
				if (!msisdn || !code)
					return notifyResult({ ok: false, error: _('Number and OTP required.') });
				return callBitsxl([ 'json', 'otp', msisdn, code ]).then((res) => {
					notifyResult(res, _('Login saved.'));
					if (res && res.ok)
						window.location.reload();
				});
			}
		}, _('Submit'))
	]);
	const resend = E('button', {
		'class': 'btn cbi-button cbi-button-apply',
		'click': () => {
			const msisdn = normalizeNumber(number.value);
			number.value = msisdn;
			if (!msisdn)
				return notifyResult({ ok: false, error: _('Number required.') });
			resend.disabled = true;
			return callBitsxl([ 'json', 'login', msisdn ]).then((res) => {
				resend.disabled = false;
				notifyResult(res, _('OTP requested.'));
				if (res && res.ok) {
					otpBox.style.display = 'block';
					resend.textContent = _('Resend OTP');
					otp.focus();
				}
			});
		}
	}, _('Resend OTP'));
	const cancel = E('button', {
		'class': 'btn cbi-button',
		'style': 'white-space:nowrap;padding-left:.65em;padding-right:.65em',
		'click': () => ui.hideModal()
	}, _('Cancel'));
	const checkAll = E('button', {
		'class': 'btn cbi-button cbi-button-save',
		'style': 'display:block;width:100%;margin-top:.75em;background:#16803a;border-color:#16803a;color:#fff;font-weight:700',
		'click': () => checkAllNumberQuota(checkAll)
	}, _('Check All Number Kuota'));

	return E('div', { 'style': 'min-width:0;width:100%;text-align:left' }, [
		E('label', { 'style': 'display:block;margin-bottom:.25em;font-weight:600' }, _('Add account')),
		E('div', { 'style': 'display:grid;grid-template-columns:minmax(6em,10em) max-content max-content;justify-content:start;gap:.35em;align-items:center;max-width:100%' }, [ number, resend, cancel ]),
		otpBox,
		checkAll
	]);
}

function numberSection(accounts, currentNumber) {
	return E('div', { 'class': 'cbi-section' }, [
		E('div', { 'style': 'text-align:center;margin-top:1em;margin-bottom:1em' }, [
			statLabel(_('Number')),
			accountSwitchButton(accounts, currentNumber || '')
		])
	]);
}

function sessionNotice(data) {
	const error = data && data.error ? data.error : _('No active account.');
	const message = error === 'refresh failed'
		? _('Session expired. Tap Number, choose Login, then request OTP again.')
		: error;
	return E('div', { 'class': 'alert-message warning' }, message);
}

return view.extend({
	load() {
		return Promise.all([
			callBitsxl([ 'json', 'dashboard' ])
		]);
	},

	render(results) {
		pageTitle(_('Dashboard'));
		const data = results[0] || {};
		const accounts = {
			lazy: true,
			accounts: data.number ? [ { number: data.number, subscription_type: data.subscription_type, active: true } ] : []
		};
		const balanceRaw = data.balance || {};
		const quotaRaw = data.quota || {};
		const balanceError = payloadErrorText(_('Balance'), balanceRaw);
		const quotaError = payloadErrorText(_('Quota'), quotaRaw);
		const payloadErrors = [ balanceError, quotaError ].filter(Boolean);
		const balance = balanceData(data);
		const quota = quotaData(data);
		const summary = quotaSummary(quota);
		return E('div', { 'class': 'cbi-map' }, [
			data.ok ? E('div', { 'class': 'cbi-section' }, [
				E('div', { 'style': 'text-align:center;margin-bottom:1em' }, [
					statLabel(_('Number')),
					accountSwitchButton(accounts, data.number)
				]),
				E('div', { 'style': 'display:flex;gap:.65em;flex-wrap:wrap;align-items:stretch' }, [
					balanceStatCard(balance),
					statCard([
						statLabel(_('Quota')),
						E('div', { 'style': 'font-size:clamp(1.05em,3.5vw,1.3em);font-weight:600;margin-bottom:.35em;white-space:nowrap' }, _('Total') + ' ' + fmtBytes(summary.total)),
						summaryBar(summary.percent),
						E('div', { 'style': 'margin-top:.35em;color:#8b949e;font-size:clamp(.85em,3vw,1em);line-height:1.25' }, '%s tersisa · %d%%'.format(fmtBytes(summary.remaining), summary.percent))
					])
				]),
				tieringMiniPanel()
			]) : numberSection(accounts, data.number),
			data.ok ? '' : sessionNotice(data),
			data.ok ? payloadErrorNotice(payloadErrors, { balance: balanceRaw, quota: quotaRaw }) : '',
			data.ok ? E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Quota Detail')),
				...quotaPackageCards(quota, quotaError)
			]) : '',
			data.ok && !payloadErrors.length ? storeSegmentsPanel(data.subscription_type) : '',
		data.ok ? transactionSummaryPanel() : '',
		E('button', { 'class': 'btn cbi-button cbi-button-reload', 'style': 'margin-top:1em', 'click': () => window.location.reload() }, _('Refresh'))
		]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
