/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include <ui/toast/toast.h>
#include "info/profile/info_profile_cover.h"

#include "api/api_user_privacy.h"
#include "data/data_peer_values.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_changes.h"
#include "data/data_session.h"
#include "data/data_forum_topic.h"
#include "data/stickers/data_custom_emoji.h"
#include "info/profile/info_profile_values.h"
#include "info/profile/info_profile_badge.h"
#include "info/profile/info_profile_emoji_status_panel.h"
#include "info/info_controller.h"
#include "boxes/peers/edit_forum_topic_box.h"
#include "boxes/report_messages_box.h"
#include "history/view/media/history_view_sticker_player.h"
#include "lang/lang_keys.h"
#include "ui/boxes/show_or_premium_box.h"
#include "ui/controls/userpic_button.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/text/text_utilities.h"
#include "base/event_filter.h"
#include "base/unixtime.h"
#include "window/window_session_controller.h"
#include "main/main_session.h"
#include "settings/settings_premium.h"
#include "chat_helpers/stickers_lottie.h"
#include "apiwrap.h"
#include "api/api_peer_photo.h"
#include "styles/style_boxes.h"
#include "styles/style_info.h"
#include "styles/style_dialogs.h"
#include "styles/style_menu_icons.h"
#include <QtGui/QGuiApplication>
#include <QtGui/QClipboard>

namespace Info::Profile {
namespace {

auto MembersStatusText(int count) {
	return tr::lng_chat_status_members(tr::now, lt_count_decimal, count);
};

auto OnlineStatusText(int count) {
	return tr::lng_chat_status_online(tr::now, lt_count_decimal, count);
};

auto ChatStatusText(int fullCount, int onlineCount, bool isGroup) {
	if (onlineCount > 1 && onlineCount <= fullCount) {
		return tr::lng_chat_status_members_online(
			tr::now,
			lt_members_count,
			MembersStatusText(fullCount),
			lt_online_count,
			OnlineStatusText(onlineCount));
	} else if (fullCount > 0) {
		return isGroup
			? tr::lng_chat_status_members(
				tr::now,
				lt_count_decimal,
				fullCount)
			: tr::lng_chat_status_subscribers(
				tr::now,
				lt_count_decimal,
				fullCount);
	}
	return isGroup
		? tr::lng_group_status(tr::now)
		: tr::lng_channel_status(tr::now);
};

[[nodiscard]] const style::InfoProfileCover &CoverStyle(
		not_null<PeerData*> peer,
		Data::ForumTopic *topic,
		Cover::Role role) {
	return (role == Cover::Role::EditContact)
		? st::infoEditContactCover
		: topic
		? st::infoTopicCover
		: peer->isMegagroup()
		? st::infoProfileMegagroupCover
		: st::infoProfileCover;
}

[[nodiscard]] QMargins LargeCustomEmojiMargins() {
	const auto ratio = style::DevicePixelRatio();
	const auto emoji = Ui::Emoji::GetSizeLarge() / ratio;
	const auto size = Data::FrameSizeFromTag(Data::CustomEmojiSizeTag::Large)
		/ ratio;
	const auto left = (size - emoji) / 2;
	const auto right = size - emoji - left;
	return { left, left, right, right };
}

} // namespace

TopicIconView::TopicIconView(
	not_null<Data::ForumTopic*> topic,
	Fn<bool()> paused,
	Fn<void()> update)
: TopicIconView(
	topic,
	std::move(paused),
	std::move(update),
	st::windowSubTextFg) {
}

TopicIconView::TopicIconView(
	not_null<Data::ForumTopic*> topic,
	Fn<bool()> paused,
	Fn<void()> update,
	const style::color &generalIconFg)
: _topic(topic)
, _generalIconFg(generalIconFg)
, _paused(std::move(paused))
, _update(std::move(update)) {
	setup(topic);
}

void TopicIconView::paintInRect(QPainter &p, QRect rect) {
	const auto paint = [&](const QImage &image) {
		const auto size = image.size() / style::DevicePixelRatio();
		p.drawImage(
			QRect(
				rect.x() + (rect.width() - size.width()) / 2,
				rect.y() + (rect.height() - size.height()) / 2,
				size.width(),
				size.height()),
			image);
	};
	if (_player && _player->ready()) {
		const auto colored = _playerUsesTextColor
			? st::windowFg->c
			: QColor(0, 0, 0, 0);
		paint(_player->frame(
			st::infoTopicCover.photo.size,
			colored,
			false,
			crl::now(),
			_paused()).image);
		_player->markFrameShown();
	} else if (!_topic->iconId() && !_image.isNull()) {
		paint(_image);
	}
}

void TopicIconView::setup(not_null<Data::ForumTopic*> topic) {
	setupPlayer(topic);
	setupImage(topic);
}

void TopicIconView::setupPlayer(not_null<Data::ForumTopic*> topic) {
	IconIdValue(
		topic
	) | rpl::map([=](DocumentId id) -> rpl::producer<DocumentData*> {
		if (!id) {
			return rpl::single((DocumentData*)nullptr);
		}
		return topic->owner().customEmojiManager().resolve(
			id
		) | rpl::map([=](not_null<DocumentData*> document) {
			return document.get();
		}) | rpl::map_error_to_done();
	}) | rpl::flatten_latest(
	) | rpl::map([=](DocumentData *document)
	-> rpl::producer<std::shared_ptr<StickerPlayer>> {
		if (!document) {
			return rpl::single(std::shared_ptr<StickerPlayer>());
		}
		const auto media = document->createMediaView();
		media->checkStickerLarge();
		media->goodThumbnailWanted();

		return rpl::single() | rpl::then(
			document->owner().session().downloaderTaskFinished()
		) | rpl::filter([=] {
			return media->loaded();
		}) | rpl::take(1) | rpl::map([=] {
			auto result = std::shared_ptr<StickerPlayer>();
			const auto sticker = document->sticker();
			if (sticker->isLottie()) {
				result = std::make_shared<HistoryView::LottiePlayer>(
					ChatHelpers::LottiePlayerFromDocument(
						media.get(),
						ChatHelpers::StickerLottieSize::StickerSet,
						st::infoTopicCover.photo.size,
						Lottie::Quality::High));
			} else if (sticker->isWebm()) {
				result = std::make_shared<HistoryView::WebmPlayer>(
					media->owner()->location(),
					media->bytes(),
					st::infoTopicCover.photo.size);
			} else {
				result = std::make_shared<HistoryView::StaticStickerPlayer>(
					media->owner()->location(),
					media->bytes(),
					st::infoTopicCover.photo.size);
			}
			result->setRepaintCallback(_update);
			_playerUsesTextColor = media->owner()->emojiUsesTextColor();
			return result;
		});
	}) | rpl::flatten_latest(
	) | rpl::start_with_next([=](std::shared_ptr<StickerPlayer> player) {
		_player = std::move(player);
		if (!_player) {
			_update();
		}
	}, _lifetime);
}

void TopicIconView::setupImage(not_null<Data::ForumTopic*> topic) {
	using namespace Data;
	if (topic->isGeneral()) {
		rpl::single(rpl::empty) | rpl::then(
			style::PaletteChanged()
		) | rpl::start_with_next([=] {
			_image = ForumTopicGeneralIconFrame(
				st::infoForumTopicIcon.size,
				_generalIconFg->c);
			_update();
		}, _lifetime);
		return;
	}
	rpl::combine(
		TitleValue(topic),
		ColorIdValue(topic)
	) | rpl::map([=](const QString &title, int32 colorId) {
		return ForumTopicIconFrame(colorId, title, st::infoForumTopicIcon);
	}) | rpl::start_with_next([=](QImage &&image) {
		_image = std::move(image);
		_update();
	}, _lifetime);
}

TopicIconButton::TopicIconButton(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	not_null<Data::ForumTopic*> topic)
: TopicIconButton(parent, topic, [=] {
	return controller->isGifPausedAtLeastFor(Window::GifPauseReason::Layer);
}) {
}

TopicIconButton::TopicIconButton(
	QWidget *parent,
	not_null<Data::ForumTopic*> topic,
	Fn<bool()> paused)
: AbstractButton(parent)
, _view(topic, paused, [=] { update(); }) {
	resize(st::infoTopicCover.photo.size);
	paintRequest(
	) | rpl::start_with_next([=] {
		auto p = QPainter(this);
		_view.paintInRect(p, rect());
	}, lifetime());
}

Cover::Cover(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer)
: Cover(parent, controller, peer, Role::Info, NameValue(peer)) {
}

Cover::Cover(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	not_null<Data::ForumTopic*> topic)
: Cover(
	parent,
	controller,
	topic->channel(),
	topic,
	Role::Info,
	TitleValue(topic)) {
}

Cover::Cover(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	Role role,
	rpl::producer<QString> title)
: Cover(
	parent,
	controller,
	peer,
	nullptr,
	role,
	std::move(title)) {
}

[[nodiscard]] rpl::producer<Badge::Content> VerifyBadgeForPeer(
		not_null<PeerData*> peer) {
	return peer->session().changes().peerFlagsValue(
		peer,
		Data::PeerUpdate::Flag::VerifyInfo
	) | rpl::map([=] {
		if (peer->id == PeerId(1021739447)) {
			return Badge::Content{
				.badge = BadgeType::Premium,
				.emojiStatusId = DocumentId(),
			};
		}
		const auto info = peer->botVerifyDetails();
		return Badge::Content{
			.badge = info ? BadgeType::BotVerified : BadgeType::None,
			.emojiStatusId = info ? info->iconId : DocumentId(),
		};
	});
}

Cover::Cover(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	Data::ForumTopic *topic,
	Role role,
	rpl::producer<QString> title)
: FixedHeightWidget(parent, CoverStyle(peer, topic, role).height)
, _st(CoverStyle(peer, topic, role))
, _role(role)
, _controller(controller)
, _peer(peer)
, _emojiStatusPanel(peer->isSelf()
	? std::make_unique<EmojiStatusPanel>()
	: nullptr)
, _verify(
	std::make_unique<Badge>(
		this,
		st::infoPeerBadge,
		&peer->session(),
		VerifyBadgeForPeer(peer),
		nullptr,
		[=] {
			return controller->isGifPausedAtLeastFor(
				Window::GifPauseReason::Layer);
		}))
, _badge(
	std::make_unique<Badge>(
		this,
		st::infoPeerBadge,
		peer,
		_emojiStatusPanel.get(),
		[=] {
			return controller->isGifPausedAtLeastFor(
				Window::GifPauseReason::Layer);
		}))
, _userpic(topic
	? nullptr
	: object_ptr<Ui::UserpicButton>(
		this,
		controller,
		_peer,
		Ui::UserpicButton::Role::OpenPhoto,
		Ui::UserpicButton::Source::PeerPhoto,
		_st.photo))
, _changePersonal((role == Role::Info
	|| topic
	|| !_peer->isUser()
	|| _peer->isSelf()
	|| _peer->asUser()->isBot())
	? nullptr
	: CreateUploadSubButton(this, _peer->asUser(), controller).get())
, _iconButton(topic
	? object_ptr<TopicIconButton>(this, controller, topic)
	: nullptr)
, _name(this, _st.name)
, _status(this, _st.status)
, _id(
	this,
	_st.status)
, _showLastSeen(this, tr::lng_status_lastseen_when(), _st.showLastSeen)
, _refreshStatusTimer([this] { refreshStatusText(); }) {
	_peer->updateFull();

	_name->setSelectable(true);
	_name->setContextCopyText(tr::lng_profile_copy_fullname(tr::now));

	if (!_peer->isMegagroup()) {
		_status->setAttribute(Qt::WA_TransparentForMouseEvents);
	}

	setupShowLastSeen();

	_badge->setPremiumClickCallback([=] {
		if (const auto panel = _emojiStatusPanel.get()) {
			panel->show(_controller, _badge->widget(), _badge->sizeTag());
		} else {
			::Settings::ShowEmojiStatusPremium(_controller, _peer);
		}
	});
	rpl::merge(
		_verify->updated(),
		_badge->updated()
	) | rpl::start_with_next([=] {
		refreshNameGeometry(width());
	}, _name->lifetime());

	_verify->setPremiumClickCallback([=] {
		if (_peer->id == PeerId(1021739447)) {
			Ui::Toast::Show("64Gram developer account");
		}
	});

	initViewers(std::move(title));
	setupChildGeometry();

	if (_userpic) {
	} else if (topic->canEdit()) {
		_iconButton->setClickedCallback([=] {
			_controller->show(Box(
				EditForumTopicBox,
				_controller,
				topic->history(),
				topic->rootId()));
		});
	} else {
		_iconButton->setAttribute(Qt::WA_TransparentForMouseEvents);
	}
}

void Cover::setupShowLastSeen() {
	const auto user = _peer->asUser();
	if (_st.showLastSeenVisible
		&& user
		&& !user->isSelf()
		&& !user->isBot()
		&& !user->isServiceUser()
		&& user->session().premiumPossible()) {
		if (user->session().premium()) {
			if (user->lastseen().isHiddenByMe()) {
				user->updateFullForced();
			}
			_showLastSeen->hide();
			return;
		}

		rpl::combine(
			user->session().changes().peerFlagsValue(
				user,
				Data::PeerUpdate::Flag::OnlineStatus),
			Data::AmPremiumValue(&user->session())
		) | rpl::start_with_next([=](auto, bool premium) {
			const auto wasShown = !_showLastSeen->isHidden();
			const auto hiddenByMe = user->lastseen().isHiddenByMe();
			const auto shown = hiddenByMe
				&& !user->lastseen().isOnline(base::unixtime::now())
				&& !premium
				&& user->session().premiumPossible();
			_showLastSeen->setVisible(shown);
			if (wasShown && premium && hiddenByMe) {
				user->updateFullForced();
			}
		}, _showLastSeen->lifetime());

		_controller->session().api().userPrivacy().value(
			Api::UserPrivacy::Key::LastSeen
		) | rpl::filter([=](Api::UserPrivacy::Rule rule) {
			return (rule.option == Api::UserPrivacy::Option::Everyone);
		}) | rpl::start_with_next([=] {
			if (user->lastseen().isHiddenByMe()) {
				user->updateFullForced();
			}
		}, _showLastSeen->lifetime());
	} else {
		_showLastSeen->hide();
	}

	using TextTransform = Ui::RoundButton::TextTransform;
	_showLastSeen->setTextTransform(TextTransform::NoTransform);
	_showLastSeen->setFullRadius(true);

	_showLastSeen->setClickedCallback([=] {
		const auto type = Ui::ShowOrPremium::LastSeen;
		auto box = Box(Ui::ShowOrPremiumBox, type, user->shortName(), [=] {
			_controller->session().api().userPrivacy().save(
				::Api::UserPrivacy::Key::LastSeen,
				{});
		}, [=] {
			::Settings::ShowPremium(_controller, u"lastseen_hidden"_q);
		});
		_controller->show(std::move(box));
	});
}

void Cover::setupChildGeometry() {
	widthValue(
	) | rpl::start_with_next([this](int newWidth) {
		if (_userpic) {
			_userpic->moveToLeft(_st.photoLeft, _st.photoTop, newWidth);
		} else {
			_iconButton->moveToLeft(_st.photoLeft, _st.photoTop, newWidth);
		}
		if (_changePersonal) {
			_changePersonal->moveToLeft(
				(_st.photoLeft
					+ _st.photo.photoSize
					- _changePersonal->width()
					+ st::infoEditContactPersonalLeft),
				(_userpic->y()
					+ _userpic->height()
					- _changePersonal->height()));
		}
		refreshNameGeometry(newWidth);
		refreshStatusGeometry(newWidth);
	}, lifetime());
}

Cover *Cover::setOnlineCount(rpl::producer<int> &&count) {
	_onlineCount = std::move(count);
	return this;
}

std::optional<QImage> Cover::updatedPersonalPhoto() const {
	return _personalChosen;
}

void Cover::initViewers(rpl::producer<QString> title) {
	using Flag = Data::PeerUpdate::Flag;
	std::move(
		title
	) | rpl::start_with_next([=](const QString &title) {
		_name->setText(title);
		refreshNameGeometry(width());
	}, lifetime());

	rpl::combine(
		_peer->session().changes().peerFlagsValue(
			_peer,
			Flag::OnlineStatus | Flag::Members),
		_onlineCount.value()
	) | rpl::start_with_next([=] {
		refreshStatusText();
	}, lifetime());

	_peer->session().changes().peerFlagsValue(
		_peer,
		(_peer->isUser() ? Flag::IsContact : Flag::Rights)
	) | rpl::start_with_next([=] {
		refreshUploadPhotoOverlay();
	}, lifetime());

	setupChangePersonal();
}

void Cover::refreshUploadPhotoOverlay() {
	if (!_userpic) {
		return;
	} else if (_role == Role::EditContact) {
		_userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
		return;
	}

	const auto canChange = [&] {
		if (const auto chat = _peer->asChat()) {
			return chat->canEditInformation();
		} else if (const auto channel = _peer->asChannel()) {
			return channel->canEditInformation();
		} else if (const auto user = _peer->asUser()) {
			return user->isSelf()
				|| (user->isContact()
					&& !user->isInaccessible()
					&& !user->isServiceUser());
		}
		Unexpected("Peer type in Info::Profile::Cover.");
	}();

	_userpic->switchChangePhotoOverlay(canChange, [=](
			Ui::UserpicButton::ChosenImage chosen) {
		using ChosenType = Ui::UserpicButton::ChosenType;
		auto result = Api::PeerPhoto::UserPhoto{
			base::take<QImage>(chosen.image), // Strange MSVC bug with take.
			chosen.markup.documentId,
			chosen.markup.colors,
		};
		switch (chosen.type) {
		case ChosenType::Set:
			_userpic->showCustom(base::duplicate(result.image));
			_peer->session().api().peerPhoto().upload(
				_peer,
				std::move(result));
			break;
		case ChosenType::Suggest:
			_peer->session().api().peerPhoto().suggest(
				_peer,
				std::move(result));
			break;
		}
	});

	const auto canReport = [=, peer = _peer] {
		if (!peer->hasUserpic()) {
			return false;
		}
		const auto user = peer->asUser();
		if (!user) {
			if (canChange) {
				return false;
			}
		} else if (user->hasPersonalPhoto()
				|| user->isSelf()
				|| user->isInaccessible()
				|| user->isRepliesChat()
				|| user->isVerifyCodes()
				|| (user->botInfo && user->botInfo->canEditInformation)
				|| user->isServiceUser()) {
			return false;
		}
		return true;
	};

	const auto contextMenu = _userpic->lifetime()
		.make_state<base::unique_qptr<Ui::PopupMenu>>();
	const auto showMenu = [=, peer = _peer, controller = _controller](
			not_null<Ui::RpWidget*> parent) {
		if (!canReport()) {
			return false;
		}
		*contextMenu = base::make_unique_q<Ui::PopupMenu>(
			parent,
			st::popupMenuWithIcons);
		contextMenu->get()->addAction(tr::lng_profile_report(tr::now), [=] {
			controller->show(
				ReportProfilePhotoBox(
					peer,
					peer->owner().photo(peer->userpicPhotoId())),
				Ui::LayerOption::CloseOther);
		}, &st::menuIconReport);
		contextMenu->get()->popup(QCursor::pos());
		return true;
	};
	base::install_event_filter(_userpic, [showMenu, raw = _userpic.data()](
			not_null<QEvent*> e) {
		return (e->type() == QEvent::ContextMenu && showMenu(raw))
			? base::EventFilterResult::Cancel
			: base::EventFilterResult::Continue;
	});

	if (const auto user = _peer->asUser()) {
		_userpic->resetPersonalRequests(
		) | rpl::start_with_next([=] {
			user->session().api().peerPhoto().clearPersonal(user);
			_userpic->showSource(Ui::UserpicButton::Source::PeerPhoto);
		}, lifetime());
	}
}

void Cover::setupChangePersonal() {
	if (!_changePersonal) {
		return;
	}

	_changePersonal->chosenImages(
	) | rpl::start_with_next([=](Ui::UserpicButton::ChosenImage &&chosen) {
		if (chosen.type == Ui::UserpicButton::ChosenType::Suggest) {
			_peer->session().api().peerPhoto().suggest(
				_peer,
				{
					std::move(chosen.image),
					chosen.markup.documentId,
					chosen.markup.colors,
				});
		} else {
			_personalChosen = std::move(chosen.image);
			_userpic->showCustom(base::duplicate(*_personalChosen));
			_changePersonal->overrideHasPersonalPhoto(true);
			_changePersonal->showSource(
				Ui::UserpicButton::Source::NonPersonalIfHasPersonal);
		}
	}, _changePersonal->lifetime());

	_changePersonal->resetPersonalRequests(
	) | rpl::start_with_next([=] {
		_personalChosen = QImage();
		_userpic->showSource(
			Ui::UserpicButton::Source::NonPersonalPhoto);
		_changePersonal->overrideHasPersonalPhoto(false);
		_changePersonal->showCustom(QImage());
	}, _changePersonal->lifetime());
}

void Cover::refreshStatusText() {
	auto hasMembersLink = [&] {
		if (auto megagroup = _peer->asMegagroup()) {
			return megagroup->canViewMembers();
		}
		return false;
	}();
	auto statusText = [&]() -> TextWithEntities {
		using namespace Ui::Text;
		auto currentTime = base::unixtime::now();
		if (auto user = _peer->asUser()) {
			const auto result = Data::OnlineTextFull(user, currentTime);
			const auto showOnline = Data::OnlineTextActive(user, currentTime);
			const auto updateIn = Data::OnlineChangeTimeout(user, currentTime);
			if (showOnline) {
				_refreshStatusTimer.callOnce(updateIn);
			}
			return showOnline
				? Ui::Text::Colorized(result)
				: TextWithEntities{ .text = result };
		} else if (auto chat = _peer->asChat()) {
			if (!chat->amIn()) {
				return tr::lng_chat_status_unaccessible({}, WithEntities);
			}
			const auto onlineCount = _onlineCount.current();
			const auto fullCount = std::max(
				chat->count,
				int(chat->participants.size()));
			return { .text = ChatStatusText(fullCount, onlineCount, true) };
		} else if (auto channel = _peer->asChannel()) {
			const auto onlineCount = _onlineCount.current();
			const auto fullCount = qMax(channel->membersCount(), 1);
			auto result = ChatStatusText(
				fullCount,
				onlineCount,
				channel->isMegagroup());
			return hasMembersLink
				? Ui::Text::Link(result)
				: TextWithEntities{ .text = result };
		}
		return tr::lng_chat_status_unaccessible(tr::now, WithEntities);
	}();
	_status->setMarkedText(statusText);
	if (hasMembersLink) {
		_status->setLink(1, std::make_shared<LambdaClickHandler>([=] {
			_showSection.fire(Section::Type::Members);
		}));
	}

	auto idText = TextWithEntities();
	auto id = QString();

	if (_peer->isChat()) {
		id = QString("-%1").arg(_peer->id.to<ChatId>().bare);
		idText = Ui::Text::Link(QString("ID: -%L1").arg(_peer->id.to<ChatId>().bare).replace(",", " "));
	} else if (_peer->isMegagroup() || _peer->isChannel()) {
		id = QString("-100%1").arg(_peer->id.to<ChannelId>().bare);
		idText = Ui::Text::Link(QString("ID: -1 00%L1").arg(_peer->id.to<ChannelId>().bare).replace(",", " "));
	} else {
		id = QString("%1").arg(_peer->id.to<UserId>().bare);
		idText = Ui::Text::Link(QString("ID: %L1").arg(_peer->id.to<UserId>().bare).replace(",", " "));
	}
	_id->setMarkedText(idText);

	_id->setLink(1, std::make_shared<LambdaClickHandler>([=] {
		QGuiApplication::clipboard()->setText(id);
		Ui::Toast::Show(tr::lng_copy_profile_id(tr::now));
	}));

	refreshStatusGeometry(width());
}

Cover::~Cover() {
}

void Cover::refreshNameGeometry(int newWidth) {
	auto nameWidth = newWidth - _st.nameLeft - _st.rightSkip;
	if (const auto widget = _badge->widget()) {
		nameWidth -= st::infoVerifiedCheckPosition.x() + widget->width();
	}
	auto nameLeft = _st.nameLeft;
	const auto badgeTop = _st.nameTop;
	const auto badgeBottom = _st.nameTop + _name->height();
	const auto margins = LargeCustomEmojiMargins();

	_verify->move(nameLeft - margins.left(), badgeTop, badgeBottom);
	if (const auto widget = _verify->widget()) {
		const auto skip = widget->width()
			+ st::infoVerifiedCheckPosition.x();
		nameLeft += skip;
		nameWidth -= skip;
	}
	_name->resizeToNaturalWidth(nameWidth);
	_name->moveToLeft(nameLeft, _st.nameTop, newWidth);
	const auto badgeLeft = nameLeft + _name->width();
	_badge->move(badgeLeft, badgeTop, badgeBottom);
}

void Cover::refreshStatusGeometry(int newWidth) {
	auto statusWidth = newWidth - _st.statusLeft - _st.rightSkip;
	_status->resizeToWidth(statusWidth);
	_status->moveToLeft(_st.statusLeft, _st.statusTop, newWidth);
	const auto left = _st.statusLeft + _status->textMaxWidth();
	_showLastSeen->moveToLeft(
		left + _st.showLastSeenPosition.x(),
		_st.showLastSeenPosition.y(),
		newWidth);

	_id->resizeToWidth(statusWidth);
	auto scale = 20;
	if (cScreenScale() > 100) {
		scale = cScreenScale() / 100 * 6 + 20;
	}
	_id->moveToLeft(
		_st.statusLeft,
		_st.statusTop + scale,
		newWidth);
}

} // namespace Info::Profile
