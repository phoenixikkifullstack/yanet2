package coordinator

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"time"

	"github.com/cenkalti/backoff/v5"
	"go.uber.org/zap"
	"gopkg.in/yaml.v3"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/connectivity"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"

	commonpb "github.com/yanet-platform/yanet2/common/proto"
	"github.com/yanet-platform/yanet2/coordinator/coordinatorpb"
	"github.com/yanet-platform/yanet2/modules/route/controlplane/routepb"
	"github.com/yanet-platform/yanet2/modules/route/internal/discovery/bird"
	"github.com/yanet-platform/yanet2/modules/route/internal/rib"
)

type instanceKey struct {
	name              string
	dataplaneInstance uint32
}

// ModuleService implements the Module gRPC service for the route module.
type ModuleService struct {
	coordinatorpb.UnimplementedModuleServiceServer

	importsMu       sync.Mutex
	imports         map[instanceKey]*importHolder
	gatewayEndpoint string    // gRPC endpoint of the RouteService (gateway) for RIB updates
	quitCh          chan bool // Signals all background BIRD import loops to stop
	log             *zap.SugaredLogger
}

func NewModuleService(
	gatewayEndpoint string,
	log *zap.SugaredLogger,
) *ModuleService {
	return &ModuleService{
		imports:         make(map[instanceKey]*importHolder),
		gatewayEndpoint: gatewayEndpoint,
		quitCh:          make(chan bool),
		log:             log,
	}
}

func (m *ModuleService) SetupConfig(
	ctx context.Context,
	req *coordinatorpb.SetupConfigRequest,
) (*coordinatorpb.SetupConfigResponse, error) {
	instance := req.GetInstance()
	configName := req.GetConfigName()

	m.log.Infow("setting up configuration",
		zap.String("name", configName),
		zap.Uint32("instance", instance),
	)

	cfg := DefaultConfig()
	if err := yaml.Unmarshal(req.GetConfig(), cfg); err != nil {
		return nil, status.Errorf(codes.InvalidArgument, "failed to unmarshal configuration: %v", err)
	}

	if err := m.setupConfig(ctx, instance, configName, cfg); err != nil {
		return nil, status.Errorf(codes.Internal, "failed to setup configuration: %v", err)
	}

	return &coordinatorpb.SetupConfigResponse{}, nil
}

func (m *ModuleService) setupConfig(
	ctx context.Context,
	instance uint32,
	configName string,
	config *Config,
) error {
	conn, err := grpc.NewClient(
		m.gatewayEndpoint,
		grpc.WithTransportCredentials(insecure.NewCredentials()),
	)
	if err != nil {
		return fmt.Errorf("failed to connect to the gateway: %w", err)
	}
	client := routepb.NewRouteServiceClient(conn)
	target := &commonpb.TargetModule{
		ConfigName:        configName,
		DataplaneInstance: instance,
	}
	flushRequest := &routepb.FlushRoutesRequest{Target: target}

	// Insert and flush static routes first.
	for _, route := range config.Routes {
		request := &routepb.InsertRouteRequest{
			Target:      target,
			Prefix:      route.Prefix.String(),
			NexthopAddr: route.Nexthop.String(),
		}

		if _, err = client.InsertRoute(ctx, request); err != nil {
			return fmt.Errorf("failed to insert static route: %w", err)
		}
	}

	if _, err = client.FlushRoutes(ctx, flushRequest); err != nil {
		return fmt.Errorf("failed to flush static routes for %s: %w", configName, err)
	}

	if len(config.BirdImport.Sockets) == 0 {
		// We do not need this connection if there is no background stream for import
		_ = conn.Close()
		return nil
	}

	// And then add dynamic routes, if any.
	return m.processBirdImport(conn, config.BirdImport, target)
}

var errStreamClosed = fmt.Errorf("stream closed")

// importHolder bundles resources for one BIRD import: the BIRD data reader,
// a cancellable context for its goroutines, the gRPC connection to the RIB service,
// and the active gRPC stream for sending updates.
type importHolder struct {
	export        *bird.Export                                                       // Reads/parses routes from BIRD
	cancel        context.CancelFunc                                                 // Stops this import's goroutines (runBirdImportLoop, export.Run)
	conn          *grpc.ClientConn                                                   // gRPC connection to RouteService (gateway)
	currentStream *grpc.ClientStreamingClient[routepb.Update, routepb.UpdateSummary] // Active gRPC stream for RIB updates; replaced on reconnect
}

// processBirdImport streams BIRD route updates to the control plane RIB.
// Handles automatic reconnection and graceful cleanup of existing imports.
// It establishes the initial gRPC stream to the RouteService (gateway), sets up
// callbacks for the bird.Export reader, and manages replacement of existing imports.
func (m *ModuleService) processBirdImport(conn *grpc.ClientConn, cfg *bird.Config, target *commonpb.TargetModule) error {
	// streamCtx governs this specific import's gRPC stream and BIRD reader.
	// Cancelled via holder.cancel on replacement or service stop.
	streamCtx, cancel := context.WithCancel(context.Background())
	client := routepb.NewRouteServiceClient(conn)
	flushRequest := &routepb.FlushRoutesRequest{Target: target}

	stream, err := client.FeedRIB(streamCtx)
	if err != nil {
		cancel() // cleanup context if stream setup fails
		return fmt.Errorf("failed to setup initial BIRD import stream: %w", err)
	}

	holder := new(importHolder)
	holder.currentStream = &stream
	log := m.log.With("config", target.ConfigName, "instance", target.DataplaneInstance)

	// onUpdate sends route batches over the gRPC stream. Called by bird.Export.
	onUpdate := func(ctx context.Context, routes []rib.Route) error {
		log.Debugf("processing %d BIRD routes", len(routes))
		for idx := range routes {
			select {
			case <-ctx.Done():
				log.Warnf("update stream send cancelled: %v", ctx.Err())
				_, closeErr := (*holder.currentStream).CloseAndRecv()
				return errors.Join(ctx.Err(), closeErr, errStreamClosed) // Signal runBirdImportLoop
			default:
			}

			err := (*holder.currentStream).Send(&routepb.Update{
				Target:   target,
				IsDelete: routes[idx].ToRemove,
				Route:    routepb.FromRIBRoute(&routes[idx], false /* isBest unknown */),
			})
			if err != nil {
				// This error stops bird.Export, triggering reconnection in runBirdImportLoop
				return fmt.Errorf("send BIRD route update for %s failed: %w", routes[idx].Prefix, err)
			}
		}
		return nil
	}

	// onFlush commits updates to dataplane. Called by bird.Export.
	onFlush := func() error {
		_, err := client.FlushRoutes(streamCtx, flushRequest) // Use stream's lifecycle context
		if err != nil {
			return fmt.Errorf("flush BIRD routes failed: %w", err)
		}
		return nil
	}

	export := bird.NewExportReader(cfg, onUpdate, onFlush, log)
	key := instanceKey{name: target.ConfigName, dataplaneInstance: target.DataplaneInstance}

	// Lock to safely access and modify m.imports.
	m.importsMu.Lock()
	defer m.importsMu.Unlock()
	// Ensure only one active import per target: stop and replace if one exists.
	if oldHolder, ok := m.imports[key]; ok {
		log.Info("replacing existing BIRD import")
		if oldHolder.cancel != nil { // Defensive check
			oldHolder.cancel()
		}
		if oldHolder.conn != nil { // Defensive check
			_ = oldHolder.conn.Close()
		}
	}

	holder.export = export
	holder.cancel = cancel
	holder.conn = conn
	m.imports[key] = holder

	// Launch goroutine for BIRD reading and stream lifecycle management.
	go m.runBirdImportLoop(streamCtx, holder, client, log)

	return nil
}

// runBirdImportLoop is the main goroutine for an active BIRD import.
// It runs the BIRD data reader (holder.export.Run) and, if the reader or gRPC stream fails,
// attempts to re-establish the stream via reconnectStream.
// Terminates if its context (ctx) is cancelled or the service's quitCh is closed.
func (m *ModuleService) runBirdImportLoop(
	ctx context.Context,
	holder *importHolder,
	client routepb.RouteServiceClient,
	log *zap.SugaredLogger,
) {
	defer func() { // Cleanup on exit
		log.Info("BIRD import loop cleanup: closing connection and cancelling context")
		holder.cancel()         // Ensure BIRD reader's context is cancelled
		_ = holder.conn.Close() // Close gRPC client connection
	}()

	runBackoff := backoff.ExponentialBackOff{
		InitialInterval:     backoff.DefaultInitialInterval,
		RandomizationFactor: backoff.DefaultRandomizationFactor,
		Multiplier:          backoff.DefaultMultiplier,
		MaxInterval:         time.Minute,
	}
	runBackoff.Reset()
	backoffResetTimeout := 10 * time.Minute

	streamActive := true

	for {
		select {
		case <-ctx.Done():
			log.Infow("BIRD import loop cancelled via context", zap.Error(ctx.Err()))
			return
		case <-m.quitCh:
			log.Info("BIRD import loop stopping due to service quit signal")
			return
		default:
		}

		if holder.conn.GetState() == connectivity.Shutdown {
			log.Error("gRPC connection for BIRD import is shutdown, terminating loop")
			return
		}

		if !streamActive {
			log.Info("attempting to re-establish BIRD route update stream")
			if !m.reconnectStream(ctx, client, holder.currentStream, log) {
				log.Info("stream reconnection aborted, terminating BIRD import loop")
				return // Reconnect failed due to ctx / quitCh
			}
			streamActive = true
			log.Info("successfully re-established BIRD route update stream")
		}

		log.Info("starting BIRD export reader")
		lastRunAttempt := time.Now()
		err := holder.export.Run(ctx) // Blocking call
		if err != nil {
			log.Warnw("BIRD export reader stopped with error", zap.Error(err))
			streamActive = false // Stream needs re-establishment

			// If context cancellation caused reader to stop, exit loop
			if errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded) {
				log.Warn("BIRD export reader context cancelled, terminating loop")
				return
			}

			// If stream wasn't closed by onUpdate's error path, try to close it here
			if !errors.Is(err, errStreamClosed) {
				log.Info("closing client stream after BIRD export reader error")
				if _, closeErr := (*holder.currentStream).CloseAndRecv(); closeErr != nil {
					log.Warnw("error closing client stream post-reader failure", zap.Error(closeErr))
				}
			}

			if time.Since(lastRunAttempt) > backoffResetTimeout {
				runBackoff.Reset()
			}
			// Apply exponential backoff before retrying the export reader
			select {
			case <-ctx.Done():
				log.Infow("BIRD import loop cancelled via context", zap.Error(ctx.Err()))
				return
			case <-m.quitCh:
				log.Info("BIRD import loop stopping due to service quit signal")
				return
			case <-time.After(runBackoff.NextBackOff()):
			}
			// Loop continues to attempt reconnection unless ctx/quitCh terminates it
		} else {
			log.Info("BIRD export reader stopped cleanly, terminating loop")
			return
		}
	}
}

// reconnectStream attempts to re-establish the gRPC stream with exponential backoff.
// Returns true if reconnection succeeds, false if aborted by context or quit signal.
// Updates `currentStream` with the new stream on success.
func (m *ModuleService) reconnectStream(
	ctx context.Context,
	client routepb.RouteServiceClient,
	currentStream *grpc.ClientStreamingClient[routepb.Update, routepb.UpdateSummary],
	log *zap.SugaredLogger,
) bool {
	log.Info("attempting to re-establish BIRD route update stream with exponential backoff")

	ticker := backoff.NewTicker(&backoff.ExponentialBackOff{
		InitialInterval:     backoff.DefaultInitialInterval,
		RandomizationFactor: backoff.DefaultRandomizationFactor,
		Multiplier:          backoff.DefaultMultiplier,
		MaxInterval:         30 * time.Second,
	})
	defer ticker.Stop()

	for {
		select {
		case <-m.quitCh:
			log.Warn("stream reconnection aborted due to service quit signal")
			return false
		case <-ctx.Done():
			log.Warnw("stream reconnection aborted due to import context cancellation", zap.Error(ctx.Err()))
			return false
		case <-ticker.C:
			log.Info("attempting FeedRIB call for new stream")
			newStream, err := client.FeedRIB(ctx) // Use import's context
			if err != nil {
				log.Warnw("failed to re-establish stream, retrying via ticker", zap.Error(err))
				continue // Ticker schedules next attempt
			}

			*currentStream = newStream // Update to new stream
			return true
		}
	}
}
