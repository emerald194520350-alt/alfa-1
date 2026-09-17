// SPDX-License-Identifier: GPL-3.0-or-later
using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;

namespace SporeCoop
{
    sealed class Peer
    {
        public TcpClient Client;
        public string Role;
        public long Sequence = -1;
        public double[] Position = new double[] { 0, 0, 0 };
        public long ModelInstance, ModelType, ModelGroup, CellResource;
        public double Scale = 1, TargetSize = 1, Opacity = 1;
        public long EditorSequence = -1;
        public long AppearanceSequence = -1;
        public string Appearance = "";
        public long AppearanceModelInstance, AppearanceModelType, AppearanceModelGroup;
        public readonly object SendLock = new object();
    }

    sealed class Session
    {
        public int Format = 1;
        public long Revision;
        public int Dna;
        public string Stage = "cell";
        public string Species = "";
        public bool Evolving;
        public string Editor;
        public string Fingerprint;
        public List<string> Applied = new List<string>();
        public bool ProgressInitialized;
        public int FoodProgression;
        public int PlantFoodProgression;
        public int OverPlantFoodProgression;
        public int OverAnimalFoodProgression;
        public int EvolutionPointsSpent;
        public int[] CellUnlocks = new int[13];
        public int[] CellMissions = new int[24];
        public int KillCount;
        public bool PlayerHasMoved;
        public bool PlayerHasEaten;
        public bool PartCinematicPlayed;
        public bool ShowMateButton;
        public bool FirstEditorEntry;
        public long EditorID;
        public long SpeciesSequence;
        public bool InvitePending;
        public bool InviteAccepted;
    }

    sealed class Proposal
    {
        public string Id;
        public string Blob;
        public long Revision;
        public string Owner;
    }

    sealed class Server
    {
        const int MaxFrame = 1048576;
        const int MaxSpeciesBytes = 262144;
        const int Protocol = 2;
        readonly object Gate = new object();
        readonly Dictionary<string, Peer> Peers = new Dictionary<string, Peer>();
        readonly HashSet<string> Ready = new HashSet<string>();
        readonly JavaScriptSerializer Json = NewJson();
        readonly string HostToken, GuestToken, SavePath;
        readonly TcpListener Listener;
        Session State;
        Proposal Pending;
        int Connections;

        static JavaScriptSerializer NewJson()
        {
            return new JavaScriptSerializer { MaxJsonLength = MaxFrame, RecursionLimit = 16 };
        }

        public Server(IPAddress address, int port, string hostToken, string guestToken, string savePath)
        {
            HostToken = hostToken;
            GuestToken = guestToken;
            SavePath = Path.GetFullPath(savePath);
            State = Load();
            Listener = new TcpListener(address, port);
        }

        Session Load()
        {
            if (!File.Exists(SavePath)) return new Session();
            var info = new FileInfo(SavePath);
            if (info.Length > MaxFrame * 8) throw new InvalidDataException("Session save is too large.");
            var loaded = NewJson().Deserialize<Session>(File.ReadAllText(SavePath, Encoding.UTF8));
            if (loaded == null || loaded.Format != 1 || loaded.Revision < 0 || loaded.Dna < 0 ||
                (loaded.Stage != "cell" && loaded.Stage != "creature") ||
                loaded.Applied == null || loaded.Applied.Count > 10000 ||
                (loaded.Fingerprint != null && !ValidFingerprint(loaded.Fingerprint)))
                throw new InvalidDataException("Invalid session save.");
            if (loaded.FoodProgression < 0 || loaded.PlantFoodProgression < 0 ||
                loaded.OverPlantFoodProgression < 0 || loaded.OverAnimalFoodProgression < 0 ||
                loaded.EvolutionPointsSpent < 0 || loaded.SpeciesSequence < 0 ||
                loaded.EditorID < 0 || loaded.EditorID > uint.MaxValue)
                throw new InvalidDataException("Invalid shared progress.");
            if (loaded.CellUnlocks == null) loaded.CellUnlocks = new int[13];
            if (loaded.CellMissions == null) loaded.CellMissions = new int[24];
            if (loaded.CellUnlocks.Length != 13)
                throw new InvalidDataException("Invalid shared unlocks.");
            if (loaded.CellMissions.Length != 24 || loaded.KillCount < 0)
                throw new InvalidDataException("Invalid shared cell missions.");
            foreach (int value in loaded.CellUnlocks)
                if (value < 0) throw new InvalidDataException("Invalid shared unlocks.");
            foreach (int value in loaded.CellMissions)
                if (value < 0) throw new InvalidDataException("Invalid shared cell missions.");
            ValidateBlob(loaded.Species);
            loaded.Evolving = false;
            loaded.Editor = null;
            loaded.InvitePending = false;
            loaded.InviteAccepted = false;
            return loaded;
        }

        // Only protocol state is saved here. Spore's own save format is not yet integrated.
        void Persist()
        {
            Directory.CreateDirectory(Path.GetDirectoryName(SavePath));
            string temp = SavePath + ".new";
            File.WriteAllText(temp, Json.Serialize(State), new UTF8Encoding(false));
            if (File.Exists(SavePath)) File.Replace(temp, SavePath, SavePath + ".bak");
            else File.Move(temp, SavePath);
        }

        object Snapshot()
        {
            var players = new Dictionary<string, object>();
            foreach (var item in Peers)
                players[item.Key] = new { position = item.Value.Position, sequence = item.Value.Sequence };
            return new { type = "state", protocol = Protocol, revision = State.Revision, dna = State.Dna,
                stage = State.Stage, species = State.Species, evolving = State.Evolving,
                invitePending = State.InvitePending, inviteAccepted = State.InviteAccepted,
                editor = State.Editor, editorId = State.EditorID,
                speciesSequence = State.SpeciesSequence,
                progressInitialized = State.ProgressInitialized,
                progress = new {
                    food = State.FoodProgression,
                    plantFood = State.PlantFoodProgression,
                    overPlantFood = State.OverPlantFoodProgression,
                    overAnimalFood = State.OverAnimalFoodProgression,
                    spent = State.EvolutionPointsSpent,
                    unlocks = State.CellUnlocks,
                    missions = State.CellMissions,
                    killCount = State.KillCount,
                    playerHasMoved = State.PlayerHasMoved,
                    playerHasEaten = State.PlayerHasEaten,
                    partCinematicPlayed = State.PartCinematicPlayed,
                    showMateButton = State.ShowMateButton,
                    firstEditorEntry = State.FirstEditorEntry
                }, ready = new List<string>(Ready), players = players };
        }

        void Send(Peer peer, object value)
        {
            var bytes = Encoding.UTF8.GetBytes(Json.Serialize(value));
            if (bytes.Length > MaxFrame) throw new InvalidDataException("Reply too large.");
            var prefix = BitConverter.GetBytes(bytes.Length);
            if (!BitConverter.IsLittleEndian) Array.Reverse(prefix);
            lock (peer.SendLock)
            {
                var stream = peer.Client.GetStream();
                stream.Write(prefix, 0, prefix.Length);
                stream.Write(bytes, 0, bytes.Length);
            }
        }

        void Broadcast(object value)
        {
            foreach (var peer in Peers.Values)
                try { Send(peer, value); } catch (IOException) { peer.Client.Close(); }
                catch (SocketException) { peer.Client.Close(); }
                catch (InvalidOperationException) { peer.Client.Close(); }
        }

        static byte[] ReadExact(NetworkStream stream, int count)
        {
            var bytes = new byte[count];
            int offset = 0;
            while (offset < count)
            {
                int read = stream.Read(bytes, offset, count - offset);
                if (read == 0) throw new EndOfStreamException();
                offset += read;
            }
            return bytes;
        }

        static Dictionary<string, object> Read(Peer peer)
        {
            var stream = peer.Client.GetStream();
            byte[] prefix = ReadExact(stream, 4);
            if (!BitConverter.IsLittleEndian) Array.Reverse(prefix);
            int length = BitConverter.ToInt32(prefix, 0);
            if (length < 2 || length > MaxFrame) throw new InvalidDataException("Invalid frame size.");
            var utf8 = new UTF8Encoding(false, true);
            var text = utf8.GetString(ReadExact(stream, length));
            var message = NewJson().DeserializeObject(text) as Dictionary<string, object>;
            if (message == null) throw new InvalidDataException("Expected JSON object.");
            return message;
        }

        static string Text(Dictionary<string, object> data, string name, int limit)
        {
            object value;
            if (!data.TryGetValue(name, out value) || !(value is string) || ((string)value).Length > limit)
                throw new ArgumentException("Invalid " + name);
            return (string)value;
        }

        static long Integer(Dictionary<string, object> data, string name, long min, long max)
        {
            object value;
            if (!data.TryGetValue(name, out value) || !(value is int || value is long))
                throw new ArgumentException("Invalid " + name);
            long number = Convert.ToInt64(value);
            if (number < min || number > max) throw new ArgumentException("Invalid " + name);
            return number;
        }

        static double Number(Dictionary<string, object> data, string name, double min, double max)
        {
            object value;
            if (!data.TryGetValue(name, out value) ||
                !(value is int || value is long || value is decimal || value is double))
                throw new ArgumentException("Invalid " + name);
            double number = Convert.ToDouble(value);
            if (double.IsNaN(number) || double.IsInfinity(number) || number < min || number > max)
                throw new ArgumentException("Invalid " + name);
            return number;
        }

        static bool Boolean(Dictionary<string, object> data, string name)
        {
            object value;
            if (!data.TryGetValue(name, out value) || !(value is bool))
                throw new ArgumentException("Invalid " + name);
            return (bool)value;
        }

        static int[] IntegerArray(Dictionary<string, object> data, string name, int count, int max)
        {
            object value;
            if (!data.TryGetValue(name, out value) || !(value is object[]))
                throw new ArgumentException("Invalid " + name);
            var source = (object[])value;
            if (source.Length != count) throw new ArgumentException("Invalid " + name);
            var result = new int[count];
            for (int i = 0; i < count; i++)
            {
                if (!(source[i] is int || source[i] is long))
                    throw new ArgumentException("Invalid " + name);
                long number = Convert.ToInt64(source[i]);
                if (number < 0 || number > max) throw new ArgumentException("Invalid " + name);
                result[i] = (int)number;
            }
            return result;
        }

        static bool ValidFingerprint(string value)
        {
            if (value.Length != 64) return false;
            foreach (char c in value)
                if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f')) return false;
            return true;
        }

        static void ValidateBlob(string value)
        {
            if (value == null || value.Length > 350000) throw new ArgumentException("Invalid species");
            if (Convert.FromBase64String(value).Length > MaxSpeciesBytes)
                throw new ArgumentException("Species is too large");
        }

        string EventId(Dictionary<string, object> data)
        {
            string id = Text(data, "eventId", 64);
            if (id.Length < 1) throw new ArgumentException("Empty eventId");
            if (State.Applied.Contains(id)) throw new ArgumentException("Duplicate eventId");
            if (State.Applied.Count >= 10000) throw new ArgumentException("Session event limit reached");
            return id;
        }

        void HostOnly(Peer peer)
        {
            if (peer.Role != "host") throw new ArgumentException("Host authority required");
        }

        void Version(Dictionary<string, object> data)
        {
            long requested = Integer(data, "revision", 0, long.MaxValue);
            if (requested != State.Revision)
                throw new ArgumentException("Stale revision; request snapshot");
        }

        void Changed()
        {
            State.Revision++;
            Persist();
            Broadcast(Snapshot());
        }

        void Handle(Peer peer, Dictionary<string, object> data)
        {
            string type = Text(data, "type", 32);
            if (type == "ping") { Send(peer, new { type = "pong" }); return; }
            if (type == "snapshot") { Send(peer, Snapshot()); return; }
            if (type == "position")
            {
                if (State.Evolving || !Peers.ContainsKey("host")) throw new ArgumentException("World paused");
                long seq = Integer(data, "sequence", 0, long.MaxValue);
                if (seq <= peer.Sequence) throw new ArgumentException("Stale position sequence");
                object value;
                if (!data.TryGetValue("position", out value) || !(value is object[]))
                    throw new ArgumentException("Invalid position");
                var items = (object[])value;
                if (items.Length != 3) throw new ArgumentException("Invalid position");
                var pos = new double[3];
                for (int i = 0; i < 3; i++)
                {
                    if (!(items[i] is int || items[i] is long || items[i] is decimal || items[i] is double))
                        throw new ArgumentException("Invalid position");
                    pos[i] = Convert.ToDouble(items[i]);
                    if (double.IsNaN(pos[i]) || double.IsInfinity(pos[i]) || Math.Abs(pos[i]) > 1000000)
                        throw new ArgumentException("Invalid position");
                }
                peer.Position = pos;
                peer.Sequence = seq;
                peer.ModelInstance = Integer(data, "modelInstance", 1, uint.MaxValue);
                peer.ModelType = Integer(data, "modelType", 0, uint.MaxValue);
                peer.ModelGroup = Integer(data, "modelGroup", 0, uint.MaxValue);
                peer.CellResource = Integer(data, "cellResource", 0, uint.MaxValue);
                peer.Scale = Number(data, "scale", 0.001, 100000);
                peer.TargetSize = Number(data, "targetSize", 0.001, 100000);
                peer.Opacity = Number(data, "opacity", 0, 1);
                Broadcast(new { type = "position", role = peer.Role, sequence = seq, position = pos,
                    modelInstance = peer.ModelInstance, modelType = peer.ModelType,
                    modelGroup = peer.ModelGroup, cellResource = peer.CellResource,
                    scale = peer.Scale, targetSize = peer.TargetSize, opacity = peer.Opacity });
                return;
            }
            if (type == "appearance")
            {
                long sequence = Integer(data, "sequence", 0, long.MaxValue);
                if (sequence <= peer.AppearanceSequence)
                    throw new ArgumentException("Stale appearance sequence");
                string blob = Text(data, "appearance", 350000);
                ValidateBlob(blob);
                peer.AppearanceSequence = sequence;
                peer.Appearance = blob;
                peer.AppearanceModelInstance = Integer(data, "modelInstance", 1, uint.MaxValue);
                peer.AppearanceModelType = Integer(data, "modelType", 0, uint.MaxValue);
                peer.AppearanceModelGroup = Integer(data, "modelGroup", 0, uint.MaxValue);
                Broadcast(new { type = "appearance", role = peer.Role, sequence = sequence,
                    modelInstance = peer.AppearanceModelInstance,
                    modelType = peer.AppearanceModelType,
                    modelGroup = peer.AppearanceModelGroup,
                    appearance = peer.Appearance });
                return;
            }
            if (!Peers.ContainsKey("host")) throw new ArgumentException("Host offline");
            if (type == "invite")
            {
                HostOnly(peer);
                if (!Peers.ContainsKey("guest")) throw new ArgumentException("Guest offline");
                State.InvitePending = true;
                State.InviteAccepted = false;
                Changed();
                return;
            }
            if (type == "inviteResponse")
            {
                if (peer.Role != "guest") throw new ArgumentException("Guest authority required");
                if (!State.InvitePending) throw new ArgumentException("No pending invitation");
                bool accepted = Boolean(data, "accepted");
                State.InvitePending = false;
                State.InviteAccepted = accepted;
                Changed();
                return;
            }
            if (type == "seedProgress")
            {
                HostOnly(peer);
                if (State.ProgressInitialized) { Send(peer, Snapshot()); return; }
                State.FoodProgression = (int)Integer(data, "food", 0, 100000000);
                State.PlantFoodProgression = (int)Integer(data, "plantFood", 0, 100000000);
                State.OverPlantFoodProgression = (int)Integer(data, "overPlantFood", 0, 100000000);
                State.OverAnimalFoodProgression = (int)Integer(data, "overAnimalFood", 0, 100000000);
                State.EvolutionPointsSpent = (int)Integer(data, "spent", 0, 100000000);
                State.CellUnlocks = IntegerArray(data, "unlocks", 13, 100000000);
                State.CellMissions = IntegerArray(data, "missions", 24, 100000000);
                State.KillCount = (int)Integer(data, "killCount", 0, 100000000);
                State.PlayerHasMoved = Boolean(data, "playerHasMoved");
                State.PlayerHasEaten = Boolean(data, "playerHasEaten");
                State.PartCinematicPlayed = Boolean(data, "partCinematicPlayed");
                State.ShowMateButton = Boolean(data, "showMateButton");
                State.FirstEditorEntry = Boolean(data, "firstEditorEntry");
                State.ProgressInitialized = true;
                Changed();
                return;
            }
            if (type == "progressDelta")
            {
                if (!State.ProgressInitialized) throw new ArgumentException("Progress is not initialized");
                string id = EventId(data);
                int food = (int)Integer(data, "food", 0, 1000000);
                int plant = (int)Integer(data, "plantFood", 0, 1000000);
                int overPlant = (int)Integer(data, "overPlantFood", 0, 1000000);
                int overAnimal = (int)Integer(data, "overAnimalFood", 0, 1000000);
                int spent = (int)Integer(data, "spent", -1000000, 1000000);
                int[] unlocks = IntegerArray(data, "unlocks", 13, 100000000);
                int[] missions = IntegerArray(data, "missions", 24, 100000000);
                int killCount = (int)Integer(data, "killCount", 0, 100000000);
                bool playerHasMoved = Boolean(data, "playerHasMoved");
                bool playerHasEaten = Boolean(data, "playerHasEaten");
                bool partCinematicPlayed = Boolean(data, "partCinematicPlayed");
                bool showMateButton = Boolean(data, "showMateButton");
                bool firstEditorEntry = Boolean(data, "firstEditorEntry");
                long nextFood = Math.Max(State.FoodProgression, food);
                long nextPlant = Math.Max(State.PlantFoodProgression, plant);
                long nextOverPlant = Math.Max(State.OverPlantFoodProgression, overPlant);
                long nextOverAnimal = Math.Max(State.OverAnimalFoodProgression, overAnimal);
                long nextSpent = (long)State.EvolutionPointsSpent + spent;
                if (nextFood > int.MaxValue || nextPlant > int.MaxValue ||
                    nextOverPlant > int.MaxValue || nextOverAnimal > int.MaxValue ||
                    nextSpent < 0 || nextSpent > int.MaxValue)
                    throw new ArgumentException("Shared progress overflow");
                State.FoodProgression = (int)nextFood;
                State.PlantFoodProgression = (int)nextPlant;
                State.OverPlantFoodProgression = (int)nextOverPlant;
                State.OverAnimalFoodProgression = (int)nextOverAnimal;
                State.EvolutionPointsSpent = (int)nextSpent;
                for (int i = 0; i < 13; i++)
                    State.CellUnlocks[i] = Math.Max(State.CellUnlocks[i], unlocks[i]);
                for (int i = 0; i < 24; i++)
                    State.CellMissions[i] = Math.Max(State.CellMissions[i], missions[i]);
                State.KillCount = Math.Max(State.KillCount, killCount);
                State.PlayerHasMoved |= playerHasMoved;
                State.PlayerHasEaten |= playerHasEaten;
                State.PartCinematicPlayed |= partCinematicPlayed;
                State.ShowMateButton |= showMateButton;
                State.FirstEditorEntry |= firstEditorEntry;
                State.Applied.Add(id);
                Changed();
                return;
            }
            if (type == "editorOpen")
            {
                long editorId = Integer(data, "editorId", 0, uint.MaxValue);
                if (!State.Evolving)
                {
                    State.Evolving = true;
                    State.Editor = peer.Role;
                    State.EditorID = editorId;
                    Ready.Clear();
                    Pending = null;
                    Changed();
                }
                Broadcast(new { type = "editorOpen", role = peer.Role, editorId = editorId,
                    revision = State.Revision });
                return;
            }
            if (type == "speciesLive")
            {
                if (!State.Evolving) throw new ArgumentException("Editor is not open");
                long sequence = Integer(data, "sequence", 0, long.MaxValue);
                if (sequence <= peer.EditorSequence) throw new ArgumentException("Stale species sequence");
                string blob = Text(data, "species", 350000);
                ValidateBlob(blob);
                peer.EditorSequence = sequence;
                State.SpeciesSequence++;
                State.Species = blob;
                Broadcast(new { type = "speciesLive", role = peer.Role,
                    sequence = State.SpeciesSequence, species = blob });
                return;
            }
            if (type == "editorClose")
            {
                string blob = Text(data, "species", 350000);
                ValidateBlob(blob);
                if (!String.IsNullOrEmpty(blob))
                {
                    State.Species = blob;
                    State.SpeciesSequence++;
                }
                State.Evolving = false;
                State.Editor = null;
                State.EditorID = 0;
                Ready.Clear();
                Pending = null;
                Changed();
                Broadcast(new { type = "editorClosed", role = peer.Role,
                    sequence = State.SpeciesSequence, species = State.Species });
                return;
            }
            if (type == "requestEvolution")
            {
                if (State.Evolving) throw new ArgumentException("Already evolving");
                Send(Peers["host"], new { type = "evolutionRequested", role = peer.Role });
                Send(peer, new { type = "evolutionRequestAccepted" });
                return;
            }
            if (type == "award")
            {
                HostOnly(peer);
                Version(data);
                if (State.Evolving) throw new ArgumentException("World paused");
                string id = EventId(data);
                int amount = (int)Integer(data, "amount", 1, 100000);
                if (State.Dna > int.MaxValue - amount) throw new ArgumentException("DNA overflow");
                State.Dna += amount;
                State.Applied.Add(id);
                Changed();
                return;
            }
            if (type == "beginEvolution")
            {
                HostOnly(peer);
                Version(data);
                string owner = Text(data, "owner", 5);
                if (State.Evolving || Peers.Count != 2 || !Peers.ContainsKey(owner))
                    throw new ArgumentException("Both players must be connected and not evolving");
                State.Evolving = true;
                State.Editor = owner;
                Ready.Clear();
                Pending = null;
                Changed();
                return;
            }
            if (type == "transferEditor")
            {
                Version(data);
                string owner = Text(data, "owner", 5);
                if (!State.Evolving || (peer.Role != State.Editor && peer.Role != "host") || !Peers.ContainsKey(owner))
                    throw new ArgumentException("Editor transfer refused");
                State.Editor = owner;
                Ready.Clear();
                Pending = null;
                Changed();
                return;
            }
            if (type == "proposeEdit")
            {
                Version(data);
                if (!State.Evolving || State.Editor != peer.Role || Pending != null)
                    throw new ArgumentException("Editor is locked or proposal pending");
                string blob = Text(data, "species", 350000);
                ValidateBlob(blob);
                string id = EventId(data);
                Pending = new Proposal { Id = id, Blob = blob, Revision = State.Revision, Owner = peer.Role };
                Send(Peers["host"], new { type = "editProposed", eventId = id, role = peer.Role,
                    revision = State.Revision, species = blob });
                Send(peer, new { type = "proposalAccepted", eventId = id });
                return;
            }
            if (type == "rejectEdit")
            {
                HostOnly(peer);
                string id = Text(data, "eventId", 64);
                if (Pending == null || Pending.Id != id) throw new ArgumentException("Unknown proposal");
                Pending = null;
                Broadcast(new { type = "editRejected", eventId = id });
                return;
            }
            if (type == "applyEdit")
            {
                HostOnly(peer);
                Version(data);
                string id = EventId(data);
                int cost = (int)Integer(data, "cost", -100000, 100000);
                if (Pending == null || Pending.Id != id || Pending.Revision != State.Revision ||
                    Pending.Owner != State.Editor || !State.Evolving)
                    throw new ArgumentException("Unknown or stale proposal");
                long nextDna = (long)State.Dna - cost;
                if (nextDna < 0 || nextDna > int.MaxValue) throw new ArgumentException("Insufficient DNA or overflow");
                State.Species = Pending.Blob;
                State.Dna = (int)nextDna;
                State.Applied.Add(id);
                Pending = null;
                Ready.Clear();
                Changed();
                return;
            }
            if (type == "ready")
            {
                Version(data);
                if (!State.Evolving || Pending != null) throw new ArgumentException("Not ready to finish");
                Ready.Add(peer.Role);
                Broadcast(Snapshot());
                return;
            }
            if (type == "finishEvolution")
            {
                HostOnly(peer);
                Version(data);
                string stage = Text(data, "stage", 8);
                if (!State.Evolving || Peers.Count != 2 || Ready.Count != 2 || Pending != null ||
                    (stage != State.Stage && !(State.Stage == "cell" && stage == "creature")))
                    throw new ArgumentException("Both players must be ready; stage transition refused");
                State.Stage = stage;
                State.Evolving = false;
                State.Editor = null;
                Ready.Clear();
                Changed();
                return;
            }
            if (type == "cancelEvolution")
            {
                HostOnly(peer);
                Version(data);
                if (!State.Evolving) throw new ArgumentException("Not evolving");
                State.Evolving = false;
                State.Editor = null;
                Pending = null;
                Ready.Clear();
                Changed();
                return;
            }
            throw new ArgumentException("Unknown message type");
        }

        void Accept(object clientObject)
        {
            var peer = new Peer { Client = (TcpClient)clientObject };
            peer.Client.ReceiveTimeout = 15000;
            peer.Client.SendTimeout = 2000;
            peer.Client.NoDelay = true;
            try
            {
                var hello = Read(peer);
                if (Text(hello, "type", 32) != "hello" ||
                    Integer(hello, "protocol", Protocol, Protocol) != Protocol)
                    throw new ArgumentException("Protocol mismatch");
                string role = Text(hello, "role", 5);
                string token = Text(hello, "token", 128);
                string fingerprint = Text(hello, "fingerprint", 64);
                if ((role != "host" && role != "guest") ||
                    token != (role == "host" ? HostToken : GuestToken))
                    throw new ArgumentException("Authentication failed");
                if (!ValidFingerprint(fingerprint)) throw new ArgumentException("Invalid fingerprint");
                lock (Gate)
                {
                    if (Peers.ContainsKey(role)) throw new ArgumentException("Role already connected");
                    if (role == "guest" && !Peers.ContainsKey("host")) throw new ArgumentException("Host offline");
                    if (State.Fingerprint != null && State.Fingerprint != fingerprint)
                        throw new ArgumentException("Game/mod fingerprint mismatch");
                    State.Fingerprint = fingerprint;
                    Persist();
                    peer.Role = role;
                    Peers.Add(role, peer);
                    Send(peer, new { type = "welcome", role = role, protocol = Protocol });
                    Broadcast(Snapshot());
                    foreach (var other in Peers.Values)
                    {
                        if (other == peer || String.IsNullOrEmpty(other.Appearance)) continue;
                        Send(peer, new { type = "appearance", role = other.Role,
                            sequence = other.AppearanceSequence,
                            modelInstance = other.AppearanceModelInstance,
                            modelType = other.AppearanceModelType,
                            modelGroup = other.AppearanceModelGroup,
                            appearance = other.Appearance });
                    }
                    Console.WriteLine("Connected: " + role);
                }
                DateTime window = DateTime.UtcNow;
                int messages = 0;
                while (true)
                {
                    var data = Read(peer);
                    if ((DateTime.UtcNow - window).TotalSeconds >= 1) { window = DateTime.UtcNow; messages = 0; }
                    if (++messages > 120) throw new InvalidDataException("Message rate exceeded");
                    lock (Gate)
                    {
                        if (Peers[peer.Role] != peer) throw new IOException("Disconnected");
                        // Roll back protocol state if persistence fails; never acknowledge an unsaved mutation.
                        string before = Json.Serialize(State);
                        var oldReady = new HashSet<string>(Ready);
                        Proposal oldPending = Pending;
                        try { Handle(peer, data); }
                        catch (ArgumentException error) { Send(peer, new { type = "error", message = error.Message }); }
                        catch (FormatException) { Send(peer, new { type = "error", message = "Invalid base64 species" }); }
                        catch (IOException)
                        {
                            State = Json.Deserialize<Session>(before);
                            Ready.Clear();
                            Ready.UnionWith(oldReady);
                            Pending = oldPending;
                            throw;
                        }
                        catch (UnauthorizedAccessException)
                        {
                            State = Json.Deserialize<Session>(before);
                            Ready.Clear();
                            Ready.UnionWith(oldReady);
                            Pending = oldPending;
                            throw;
                        }
                    }
                }
            }
            catch (ArgumentException error)
            {
                lock (Gate) try { Send(peer, new { type = "error", message = error.Message }); } catch { }
            }
            catch (Exception error)
            {
                Console.WriteLine("Connection closed: " + error.GetType().Name);
            }
            finally
            {
                lock (Gate)
                {
                    peer.Client.Close();
                    if (peer.Role != null && Peers.ContainsKey(peer.Role) && Peers[peer.Role] == peer)
                    {
                        Peers.Remove(peer.Role);
                        State.Evolving = false;
                        State.Editor = null;
                        State.InvitePending = false;
                        State.InviteAccepted = false;
                        Ready.Clear();
                        Pending = null;
                        if (peer.Role == "host")
                        {
                            foreach (var guest in Peers.Values) guest.Client.Close();
                            Peers.Clear();
                        }
                        try { Changed(); } catch (Exception error) { Console.WriteLine("Save failed: " + error.GetType().Name); }
                    }
                }
                Interlocked.Decrement(ref Connections);
            }
        }

        public void Run()
        {
            Listener.Start();
            Console.WriteLine("LISTENING " + Listener.LocalEndpoint);
            while (true)
            {
                var client = Listener.AcceptTcpClient();
                if (Interlocked.Increment(ref Connections) > 4)
                {
                    client.Close();
                    Interlocked.Decrement(ref Connections);
                    continue;
                }
                ThreadPool.QueueUserWorkItem(Accept, client);
            }
        }

        static string Token()
        {
            var bytes = new byte[24];
            using (var rng = RandomNumberGenerator.Create()) rng.GetBytes(bytes);
            return BitConverter.ToString(bytes).Replace("-", "").ToLowerInvariant();
        }

        static bool PrivateAddress(IPAddress address)
        {
            if (address.AddressFamily != AddressFamily.InterNetwork) return false;
            byte[] b = address.GetAddressBytes();
            return b[0] == 127 || b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) ||
                (b[0] == 192 && b[1] == 168) || b[0] == 25 || b[0] == 26;
        }

        static int Main(string[] args)
        {
            try
            {
                var options = new Dictionary<string, string>();
                for (int i = 0; i < args.Length; i += 2)
                {
                    if (i + 1 == args.Length || (args[i] != "--listen" && args[i] != "--port" &&
                        args[i] != "--host-token" && args[i] != "--guest-token" && args[i] != "--save"))
                        throw new ArgumentException("Expected --listen, --port, --host-token, --guest-token or --save with a value");
                    options.Add(args[i], args[i + 1]);
                }
                string value;
                IPAddress address = IPAddress.Parse(options.TryGetValue("--listen", out value) ? value : "127.0.0.1");
                if (!PrivateAddress(address)) throw new ArgumentException("Bind a private IPv4 or Radmin/Hamachi 25.x/26.x address");
                int port = options.TryGetValue("--port", out value) ? int.Parse(value) : 5523;
                if (port < 1 || port > 65535) throw new ArgumentException("Invalid port");
                string hostToken = options.TryGetValue("--host-token", out value) ? value : Token();
                string guestToken = options.TryGetValue("--guest-token", out value) ? value : Token();
                if (hostToken.Length < 24 || hostToken.Length > 128 || guestToken.Length < 24 ||
                    guestToken.Length > 128 || hostToken == guestToken) throw new ArgumentException("Use distinct tokens of 24-128 characters");
                string save = options.TryGetValue("--save", out value) ? value : Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "session.json");
                Console.WriteLine("SporeCoop protocol prototype. Gameplay adapter is not implemented.");
                Console.WriteLine("HOST TOKEN (keep local): " + hostToken);
                Console.WriteLine("GUEST TOKEN (share with friend): " + guestToken);
                new Server(address, port, hostToken, guestToken, save).Run();
                return 0;
            }
            catch (Exception error)
            {
                Console.Error.WriteLine("Server failed: " + error.Message);
                return 1;
            }
        }
    }
}
