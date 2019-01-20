/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2018, Google Inc.
 *
 * media_device.cpp - Media device handler
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <string>
#include <vector>

#include <linux/media.h>

#include "log.h"
#include "media_device.h"

/**
 * \file media_device.h
 * \brief Provide a representation of a Linux kernel Media Controller device
 * that exposes the full graph topology.
 */

namespace libcamera {

LOG_DEFINE_CATEGORY(MediaDevice)

/**
 * \class MediaDevice
 * \brief The MediaDevice represents a Media Controller device with its full
 * graph of connected objects.
 *
 * A MediaDevice instance is associated with a media controller device node when
 * created, and that association is kept for the lifetime of the MediaDevice
 * instance.
 *
 * The instance is created with an empty media graph. Before performing any
 * other operation, it must be opened with the open() function and the media
 * graph populated by calling populate(). Instances of MediaEntity, MediaPad and
 * MediaLink are created to model the media graph, and stored in a map indexed
 * by object id.
 *
 * The graph is valid once successfully populated, as reported by the valid()
 * function. It can be queried to list all entities(), or entities can be
 * looked up by name with getEntityByName(). The graph can be traversed from
 * entity to entity through pads and links as exposed by the corresponding
 * classes.
 *
 * An open media device will keep an open file handle for the underlying media
 * controller device node. It can be closed at any time with a call to close().
 * This will not invalidate the media graph and all cached media objects remain
 * valid and can be accessed normally. The device can then be later reopened if
 * needed to perform other operations that interact with the device node.
 *
 * Media device can be claimed for exclusive use with acquire(), released with
 * release() and tested with busy(). This mechanism is aimed at pipeline
 * managers to claim media devices they support during enumeration.
 */

/**
 * \brief Construct a MediaDevice
 * \param devnode The media device node path
 *
 * Once constructed the media device is invalid, and must be opened and
 * populated with open() and populate() before the media graph can be queried.
 */
MediaDevice::MediaDevice(const std::string &devnode)
	: devnode_(devnode), fd_(-1), valid_(false), acquired_(false)
{
}

MediaDevice::~MediaDevice()
{
	if (fd_ != -1)
		::close(fd_);
	clear();
}

/**
 * \brief Claim a device for exclusive use
 *
 * The device claiming mechanism offers simple media device access arbitration
 * between multiple users. When the media device is created, it is available to
 * all users. Users can query the media graph to determine whether they can
 * support the device and, if they do, claim the device for exclusive use. Other
 * users are then expected to skip over media devices in use as reported by the
 * busy() function.
 *
 * Once claimed the device shall be released by its user when not needed anymore
 * by calling the release() function.
 *
 * Exclusive access is only guaranteed if all users of the media device abide by
 * the device claiming mechanism, as it isn't enforced by the media device
 * itself.
 *
 * \return true if the device was successfully claimed, or false if it was
 * already in use
 * \sa release(), busy()
 */
bool MediaDevice::acquire()
{
	if (acquired_)
		return false;

	acquired_ = true;
	return true;
}

/**
 * \fn MediaDevice::release()
 * \brief Release a device previously claimed for exclusive use
 * \sa acquire(), busy()
 */

/**
 * \fn MediaDevice::busy()
 * \brief Check if a device is in use
 * \return true if the device has been claimed for exclusive use, or false if it
 * is available
 * \sa acquire(), release()
 */

/**
 * \brief Open a media device and retrieve device information
 *
 * Before populating the media graph or performing any operation that interact
 * with the device node associated with the media device, the device node must
 * be opened.
 *
 * This function also retrieves media device information from the device node,
 * which can be queried through driver().
 *
 * If the device is already open the function returns -EBUSY.
 *
 * \return 0 on success or a negative error code otherwise
 */
int MediaDevice::open()
{
	if (fd_ != -1) {
		LOG(MediaDevice, Error) << "MediaDevice already open";
		return -EBUSY;
	}

	int ret = ::open(devnode_.c_str(), O_RDWR);
	if (ret < 0) {
		ret = -errno;
		LOG(MediaDevice, Error)
			<< "Failed to open media device at "
			<< devnode_ << ": " << strerror(-ret);
		return ret;
	}
	fd_ = ret;

	struct media_device_info info = { };
	ret = ioctl(fd_, MEDIA_IOC_DEVICE_INFO, &info);
	if (ret) {
		ret = -errno;
		LOG(MediaDevice, Error)
			<< "Failed to get media device info "
			<< ": " << strerror(-ret);
		return ret;
	}

	driver_ = info.driver;

	return 0;
}

/**
 * \brief Close the media device
 *
 * This function closes the media device node. It does not invalidate the media
 * graph and all cached media objects remain valid and can be accessed normally.
 * Once closed no operation interacting with the media device node can be
 * performed until the device is opened again.
 *
 * Closing an already closed device is allowed and will not perform any
 * operation.
 *
 * \sa open()
 */
void MediaDevice::close()
{
	if (fd_ == -1)
		return;

	::close(fd_);
	fd_ = -1;
}

/**
 * \brief Populate the media graph with media objects
 *
 * This function enumerates all media objects in the media device graph and
 * creates their MediaObject representations. All entities, pads and links are
 * stored as MediaEntity, MediaPad and MediaLink respectively, with cross-
 * references between objects. Interfaces are not processed.
 *
 * Entities are stored in a separate list in the MediaDevice to ease lookup,
 * while pads are accessible from the entity they belong to and links from the
 * pads they connect.
 *
 * \return 0 on success, a negative error code otherwise
 */
int MediaDevice::populate()
{
	struct media_v2_topology topology = { };
	struct media_v2_entity *ents = nullptr;
	struct media_v2_interface *interfaces = nullptr;
	struct media_v2_link *links = nullptr;
	struct media_v2_pad *pads = nullptr;
	__u64 version = -1;
	int ret;

	clear();

	/*
	 * Keep calling G_TOPOLOGY until the version number stays stable.
	 */
	while (true) {
		topology.topology_version = 0;
		topology.ptr_entities = reinterpret_cast<__u64>(ents);
		topology.ptr_interfaces = reinterpret_cast<__u64>(interfaces);
		topology.ptr_links = reinterpret_cast<__u64>(links);
		topology.ptr_pads = reinterpret_cast<__u64>(pads);

		ret = ioctl(fd_, MEDIA_IOC_G_TOPOLOGY, &topology);
		if (ret < 0) {
			ret = -errno;
			LOG(MediaDevice, Error)
				<< "Failed to enumerate topology: "
				<< strerror(-ret);
			return ret;
		}

		if (version == topology.topology_version)
			break;

		delete[] ents;
		delete[] interfaces;
		delete[] pads;
		delete[] links;

		ents = new struct media_v2_entity[topology.num_entities]();
		interfaces = new struct media_v2_interface[topology.num_interfaces]();
		links = new struct media_v2_link[topology.num_links]();
		pads = new struct media_v2_pad[topology.num_pads]();

		version = topology.topology_version;
	}

	/* Populate entities, pads and links. */
	if (populateEntities(topology) &&
	    populatePads(topology) &&
	    populateLinks(topology))
		valid_ = true;

	delete[] ents;
	delete[] interfaces;
	delete[] pads;
	delete[] links;

	if (!valid_) {
		clear();
		return -EINVAL;
	}

	return 0;
}

/**
 * \fn MediaDevice::valid()
 * \brief Query whether the media graph has been populated and is valid
 * \return true if the media graph is valid, false otherwise
 */

/**
 * \fn MediaDevice::driver()
 * \brief Retrieve the media device driver name
 * \return The name of the kernel driver that handles the MediaDevice
 */

/**
 * \fn MediaDevice::devnode()
 * \brief Retrieve the media device device node path
 * \return The MediaDevice devnode path
 */

/**
 * \fn MediaDevice::entities()
 * \brief Retrieve the list of entities in the media graph
 * \return The list of MediaEntities registered in the MediaDevice
 */

/**
 * \brief Return the MediaEntity with name \a name
 * \param name The entity name
 * \return The entity with \a name
 * \return nullptr if no entity with \a name is found
 */
MediaEntity *MediaDevice::getEntityByName(const std::string &name) const
{
	for (MediaEntity *e : entities_)
		if (e->name() == name)
			return e;

	return nullptr;
}

/**
 * \brief Retrieve the MediaLink connecting two pads, identified by entity
 * names and pad indexes
 * \param sourceName The source entity name
 * \param sourceIdx The index of the source pad
 * \param sinkName The sink entity name
 * \param sinkIdx The index of the sink pad
 *
 * Find the link that connects the pads at index \a sourceIdx of the source
 * entity with name \a sourceName, to the pad at index \a sinkIdx of the
 * sink entity with name \a sinkName, if any.
 *
 * \sa MediaDevice::link(const MediaEntity *source, unsigned int sourceIdx, const MediaEntity *sink, unsigned int sinkIdx) const
 * \sa MediaDevice::link(const MediaPad *source, const MediaPad *sink) const
 *
 * \return The link that connects the two pads, or nullptr if no such a link
 * exists
 */
MediaLink *MediaDevice::link(const std::string &sourceName, unsigned int sourceIdx,
			     const std::string &sinkName, unsigned int sinkIdx)
{
	const MediaEntity *source = getEntityByName(sourceName);
	const MediaEntity *sink = getEntityByName(sinkName);
	if (!source || !sink)
		return nullptr;

	return link(source, sourceIdx, sink, sinkIdx);
}

/**
 * \brief Retrieve the MediaLink connecting two pads, identified by the
 * entities they belong to and pad indexes
 * \param source The source entity
 * \param sourceIdx The index of the source pad
 * \param sink The sink entity
 * \param sinkIdx The index of the sink pad
 *
 * Find the link that connects the pads at index \a sourceIdx of the source
 * entity \a source, to the pad at index \a sinkIdx of the sink entity \a
 * sink, if any.
 *
 * \sa MediaDevice::link(const std::string &sourceName, unsigned int sourceIdx, const std::string &sinkName, unsigned int sinkIdx) const
 * \sa MediaDevice::link(const MediaPad *source, const MediaPad *sink) const
 *
 * \return The link that connects the two pads, or nullptr if no such a link
 * exists
 */
MediaLink *MediaDevice::link(const MediaEntity *source, unsigned int sourceIdx,
			     const MediaEntity *sink, unsigned int sinkIdx)
{

	const MediaPad *sourcePad = source->getPadByIndex(sourceIdx);
	const MediaPad *sinkPad = sink->getPadByIndex(sinkIdx);
	if (!sourcePad || !sinkPad)
		return nullptr;

	return link(sourcePad, sinkPad);
}

/**
 * \brief Retrieve the MediaLink that connects two pads
 * \param source The source pad
 * \param sink The sink pad
 *
 * \sa MediaDevice::link(const std::string &sourceName, unsigned int sourceIdx, const std::string &sinkName, unsigned int sinkIdx) const
 * \sa MediaDevice::link(const MediaEntity *source, unsigned int sourceIdx, const MediaEntity *sink, unsigned int sinkIdx) const
 *
 * \return The link that connects the two pads, or nullptr if no such a link
 * exists
 */
MediaLink *MediaDevice::link(const MediaPad *source, const MediaPad *sink)
{
	for (MediaLink *link : source->links()) {
		if (link->sink()->id() == sink->id())
			return link;
	}

	return nullptr;
}

/**
 * \brief Disable all links in the media device
 *
 * Disable all the media device links, clearing the MEDIA_LNK_FL_ENABLED flag
 * on links which are not flagged as IMMUTABLE.
 *
 * \return 0 on success, or a negative error code otherwise
 */
int MediaDevice::disableLinks()
{
	for (MediaEntity *entity : entities_) {
		for (MediaPad *pad : entity->pads()) {
			if (!(pad->flags() & MEDIA_PAD_FL_SOURCE))
				continue;

			for (MediaLink *link : pad->links()) {
				if (link->flags() & MEDIA_LNK_FL_IMMUTABLE)
					continue;

				int ret = link->setEnabled(false);
				if (ret)
					return ret;
			}
		}
	}

	return 0;
}

/**
 * \var MediaDevice::objects_
 * \brief Global map of media objects (entities, pads, links) keyed by their
 * object id.
 */

/**
 * \brief Retrieve the media graph object specified by \a id
 * \return The graph object, or nullptr if no object with \a id is found
 */
MediaObject *MediaDevice::object(unsigned int id)
{
	auto it = objects_.find(id);
	return (it == objects_.end()) ? nullptr : it->second;
}

/**
 * \brief Add a media object to the media graph
 *
 * If the \a object has a unique id it is added to the media graph, and its
 * lifetime will be managed by the media device. Otherwise the object isn't
 * added to the graph and the caller must delete it.
 *
 * \return true if the object was successfully added to the graph and false
 * otherwise
 */
bool MediaDevice::addObject(MediaObject *object)
{

	if (objects_.find(object->id()) != objects_.end()) {
		LOG(MediaDevice, Error)
			<< "Element with id " << object->id()
			<< " already enumerated.";
		return false;
	}

	objects_[object->id()] = object;

	return true;
}

/**
 * \brief Delete all graph objects in the media device
 *
 * Clear the media graph and delete all the objects it contains. After this
 * function returns any previously obtained pointer to a media graph object
 * becomes invalid.
 *
 * The media device graph state is reset to invalid when the graph is cleared.
 *
 * \sa valid()
 */
void MediaDevice::clear()
{
	for (auto const &o : objects_)
		delete o.second;

	objects_.clear();
	entities_.clear();
	valid_ = false;
}

/**
 * \var MediaDevice::entities_
 * \brief Global list of media entities in the media graph
 */

/**
 * \brief Find the interface associated with an entity
 * \param topology The media topology as returned by MEDIA_IOC_G_TOPOLOGY
 * \param entityId The entity id
 * \return A pointer to the interface if found, or nullptr otherwise
 */
struct media_v2_interface *MediaDevice::findInterface(const struct media_v2_topology &topology,
						      unsigned int entityId)
{
	struct media_v2_link *links = reinterpret_cast<struct media_v2_link *>
						      (topology.ptr_links);
	unsigned int ifaceId;
	unsigned int i;

	for (i = 0; i < topology.num_links; ++i) {
		/* Search for the interface to entity link. */
		if (links[i].sink_id != entityId)
			continue;

		if ((links[i].flags & MEDIA_LNK_FL_LINK_TYPE) !=
		    MEDIA_LNK_FL_INTERFACE_LINK)
			continue;

		ifaceId = links[i].source_id;
		break;
	}
	if (i == topology.num_links)
		return nullptr;

	struct media_v2_interface *ifaces = reinterpret_cast<struct media_v2_interface *>
					    (topology.ptr_interfaces);
	for (i = 0; i < topology.num_interfaces; ++i) {
		if (ifaces[i].id == ifaceId)
			return &ifaces[i];
	}

	return nullptr;
}

/*
 * For each entity in the media graph create a MediaEntity and store a
 * reference in the media device objects map and entities list.
 */
bool MediaDevice::populateEntities(const struct media_v2_topology &topology)
{
	struct media_v2_entity *mediaEntities = reinterpret_cast<struct media_v2_entity *>
						(topology.ptr_entities);

	for (unsigned int i = 0; i < topology.num_entities; ++i) {
		/*
		 * Find the interface linked to this entity to get the device
		 * node major and minor numbers.
		 */
		struct media_v2_interface *iface =
			findInterface(topology, mediaEntities[i].id);

		MediaEntity *entity;
		if (iface)
			entity = new MediaEntity(this, &mediaEntities[i],
						 iface->devnode.major,
						 iface->devnode.minor);
		else
			entity = new MediaEntity(this, &mediaEntities[i]);

		if (!addObject(entity)) {
			delete entity;
			return false;
		}

		entities_.push_back(entity);
	}

	return true;
}

bool MediaDevice::populatePads(const struct media_v2_topology &topology)
{
	struct media_v2_pad *mediaPads = reinterpret_cast<struct media_v2_pad *>
					 (topology.ptr_pads);

	for (unsigned int i = 0; i < topology.num_pads; ++i) {
		unsigned int entity_id = mediaPads[i].entity_id;

		/* Store a reference to this MediaPad in entity. */
		MediaEntity *mediaEntity = dynamic_cast<MediaEntity *>
					   (object(entity_id));
		if (!mediaEntity) {
			LOG(MediaDevice, Error)
				<< "Failed to find entity with id: "
				<< entity_id;
			return false;
		}

		MediaPad *pad = new MediaPad(&mediaPads[i], mediaEntity);
		if (!addObject(pad)) {
			delete pad;
			return false;
		}

		mediaEntity->addPad(pad);
	}

	return true;
}

bool MediaDevice::populateLinks(const struct media_v2_topology &topology)
{
	struct media_v2_link *mediaLinks = reinterpret_cast<struct media_v2_link *>
					   (topology.ptr_links);

	for (unsigned int i = 0; i < topology.num_links; ++i) {
		/*
		 * Skip links between entities and interfaces: we only care
		 * about pad-2-pad links here.
		 */
		if ((mediaLinks[i].flags & MEDIA_LNK_FL_LINK_TYPE) ==
		    MEDIA_LNK_FL_INTERFACE_LINK)
			continue;

		/* Store references to source and sink pads in the link. */
		unsigned int source_id = mediaLinks[i].source_id;
		MediaPad *source = dynamic_cast<MediaPad *>
				   (object(source_id));
		if (!source) {
			LOG(MediaDevice, Error)
				<< "Failed to find pad with id: "
				<< source_id;
			return false;
		}

		unsigned int sink_id = mediaLinks[i].sink_id;
		MediaPad *sink = dynamic_cast<MediaPad *>
				 (object(sink_id));
		if (!sink) {
			LOG(MediaDevice, Error)
				<< "Failed to find pad with id: "
				<< sink_id;
			return false;
		}

		MediaLink *link = new MediaLink(&mediaLinks[i], source, sink);
		if (!addObject(link)) {
			delete link;
			return false;
		}

		source->addLink(link);
		sink->addLink(link);
	}

	return true;
}

/**
 * \brief Apply \a flags to a link between two pads
 * \param link The link to apply flags to
 * \param flags The flags to apply to the link
 *
 * This function applies the link \a flags (as defined by the MEDIA_LNK_FL_*
 * macros from the Media Controller API) to the given \a link. It implements
 * low-level link setup as it performs no checks on the validity of the \a
 * flags, and assumes that the supplied \a flags are valid for the link (e.g.
 * immutable links cannot be disabled).
*
 * \sa MediaLink::setEnabled(bool enable)
 *
 * \return 0 on success, or a negative error code otherwise
 */
int MediaDevice::setupLink(const MediaLink *link, unsigned int flags)
{
	struct media_link_desc linkDesc = { };
	MediaPad *source = link->source();
	MediaPad *sink = link->sink();

	linkDesc.source.entity = source->entity()->id();
	linkDesc.source.index = source->index();
	linkDesc.source.flags = MEDIA_PAD_FL_SOURCE;

	linkDesc.sink.entity = sink->entity()->id();
	linkDesc.sink.index = sink->index();
	linkDesc.sink.flags = MEDIA_PAD_FL_SINK;

	linkDesc.flags = flags;

	int ret = ioctl(fd_, MEDIA_IOC_SETUP_LINK, &linkDesc);
	if (ret) {
		ret = -errno;
		LOG(MediaDevice, Error)
			<< "Failed to setup link: "
			<< strerror(-ret);
		return ret;
	}

	LOG(MediaDevice, Debug)
		<< source->entity()->name() << "["
		<< source->index() << "] -> "
		<< sink->entity()->name() << "["
		<< sink->index() << "]: " << flags;

	return 0;
}

} /* namespace libcamera */
